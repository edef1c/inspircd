/*       +------------------------------------+
 *       | Inspire Internet Relay Chat Daemon |
 *       +------------------------------------+
 *
 *  InspIRCd: (C) 2002-2009 InspIRCd Development Team
 * See: http://wiki.inspircd.org/Credits
 *
 * This program is free but copyrighted software; see
 *            the file COPYING for details.
 *
 * ---------------------------------------------------
 */

#include "inspircd.h"
#include "socket.h"
#include "xline.h"
#include "../m_hash.h"
#include "socketengine.h"

#include "main.h"
#include "../spanningtree.h"
#include "utils.h"
#include "treeserver.h"
#include "link.h"
#include "treesocket.h"
#include "resolvers.h"

/** Because most of the I/O gubbins are encapsulated within
 * BufferedSocket, we just call the superclass constructor for
 * most of the action, and append a few of our own values
 * to it.
 */
TreeSocket::TreeSocket(SpanningTreeUtilities* Util, const std::string& shost, int iport, unsigned long maxtime, const std::string &ServerName, const std::string &bindto, Autoconnect* myac, const std::string& hook)
	: Utils(Util), IP(shost), myautoconnect(myac)
{
	age = ServerInstance->Time();
	myhost = ServerName;
	capab_phase = 0;
	proto_version = 0;
	LinkState = CONNECTING;
	if (!hook.empty())
	{
		modulelist* ml = ServerInstance->Modules->FindInterface("BufferedSocketHook");
		if (ml)
		{
			for(modulelist::iterator i = ml->begin(); i != ml->end(); ++i)
			{
				std::string name = (**i).ModuleSourceFile;
				int a = name.rfind('_');
				int b = name.rfind('.');
				name = name.substr(a+1, b-a-1);
				if (name == hook)
				{
					AddIOHook(*i);
					goto found;
				}
			}
		}
		SetError("Could not find hook '" + hook + "' for connection to " + ServerName);
		return;
	}
found:
	DoConnect(shost, iport, maxtime, bindto);
	Utils->timeoutlist[this] = std::pair<std::string, int>(ServerName, maxtime);
	SendCapabilities(1);
}

/** When a listening socket gives us a new file descriptor,
 * we must associate it with a socket without creating a new
 * connection. This constructor is used for this purpose.
 */
TreeSocket::TreeSocket(SpanningTreeUtilities* Util, int newfd, ListenSocket* via, irc::sockets::sockaddrs* client, irc::sockets::sockaddrs* server)
	: BufferedSocket(newfd), Utils(Util)
{
	int dummy;
	irc::sockets::satoap(*client, IP, dummy);
	age = ServerInstance->Time();
	LinkState = WAIT_AUTH_1;
	capab_phase = 0;
	proto_version = 0;

	FOREACH_MOD(I_OnHookIO, OnHookIO(this, via));
	if (GetIOHook())
		GetIOHook()->OnStreamSocketAccept(this, client, server);
	SendCapabilities(1);

	Utils->timeoutlist[this] = std::pair<std::string, int>("inbound from " + IP, 30);
}

ServerState TreeSocket::GetLinkState()
{
	return this->LinkState;
}

void TreeSocket::CleanNegotiationInfo()
{
	ModuleList.clear();
	OptModuleList.clear();
	CapKeys.clear();
	ourchallenge.clear();
	theirchallenge.clear();
	OutboundPass.clear();
}

CullResult TreeSocket::cull()
{
	Utils->timeoutlist.erase(this);
	if (myautoconnect)
		Utils->Creator->ConnectServer(myautoconnect, false);
	return this->BufferedSocket::cull();
}

TreeSocket::~TreeSocket()
{
}

/** When an outbound connection finishes connecting, we receive
 * this event, and must send our SERVER string to the other
 * side. If the other side is happy, as outlined in the server
 * to server docs on the inspircd.org site, the other side
 * will then send back its own server string.
 */
void TreeSocket::OnConnected()
{
	if (this->LinkState == CONNECTING)
	{
		/* we do not need to change state here. */
		for (std::vector<reference<Link> >::iterator i = Utils->LinkBlocks.begin(); i < Utils->LinkBlocks.end(); ++i)
		{
			Link* x = *i;
			if (x->Name == this->myhost)
			{
				ServerInstance->SNO->WriteGlobalSno('l', "Connection to \2%s\2[%s] started.", myhost.c_str(), (x->HiddenFromStats ? "<hidden>" : this->IP.c_str()));
				this->OutboundPass = x->SendPass;
				this->SendCapabilities(1);
				return;
			}
		}
	}
	/* There is a (remote) chance that between the /CONNECT and the connection
	 * being accepted, some muppet has removed the <link> block and rehashed.
	 * If that happens the connection hangs here until it's closed. Unlikely
	 * and rather harmless.
	 */
	ServerInstance->SNO->WriteGlobalSno('l', "Connection to \2%s\2 lost link tag(!)", myhost.c_str());
}

void TreeSocket::OnError(BufferedSocketError e)
{
	ServerInstance->SNO->WriteGlobalSno('l', "Connection to \002%s\002 failed with error: %s",
		myhost.c_str(), getError().c_str());
}

void TreeSocket::SendError(const std::string &errormessage)
{
	WriteLine("ERROR :"+errormessage);
	SetError(errormessage);
	/* Display the error locally as well as sending it remotely */
	ServerInstance->SNO->WriteGlobalSno('l', "Sent \2ERROR\2 to %s: %s", (this->InboundServerName.empty() ? this->IP.c_str() : this->InboundServerName.c_str()), errormessage.c_str());
}

/** This function forces this server to quit, removing this server
 * and any users on it (and servers and users below that, etc etc).
 * It's very slow and pretty clunky, but luckily unless your network
 * is having a REAL bad hair day, this function shouldnt be called
 * too many times a month ;-)
 */
void TreeSocket::SquitServer(std::string &from, TreeServer* Current)
{
	ServerInstance->Logs->Log("m_spanningtree",DEBUG,"SquitServer for %s from %s",
		Current->GetName().c_str(), from.c_str());
	/* recursively squit the servers attached to 'Current'.
	 * We're going backwards so we don't remove users
	 * while we still need them ;)
	 */
	for (unsigned int q = 0; q < Current->ChildCount(); q++)
	{
		TreeServer* recursive_server = Current->GetChild(q);
		this->SquitServer(from,recursive_server);
	}
	/* Now we've whacked the kids, whack self */
	num_lost_servers++;
	num_lost_users += Current->QuitUsers(from);
}

/** This is a wrapper function for SquitServer above, which
 * does some validation first and passes on the SQUIT to all
 * other remaining servers.
 */
void TreeSocket::Squit(TreeServer* Current, const std::string &reason)
{
	bool LocalSquit = false;

	if ((Current) && (Current != Utils->TreeRoot))
	{
		DelServerEvent(Utils->Creator, Current->GetName());

		parameterlist params;
		params.push_back(Current->GetName());
		params.push_back(":"+reason);
		Utils->DoOneToAllButSender(Current->GetParent()->GetName(),"SQUIT",params,Current->GetName());
		if (Current->GetParent() == Utils->TreeRoot)
		{
			ServerInstance->SNO->WriteGlobalSno('l', "Server \002"+Current->GetName()+"\002 split: "+reason);
			LocalSquit = true;
		}
		else
		{
			ServerInstance->SNO->WriteGlobalSno('L', "Server \002"+Current->GetName()+"\002 split from server \002"+Current->GetParent()->GetName()+"\002 with reason: "+reason);
		}
		num_lost_servers = 0;
		num_lost_users = 0;
		std::string from = Current->GetParent()->GetName()+" "+Current->GetName();
		SquitServer(from, Current);
		Current->Tidy();
		Current->GetParent()->DelChild(Current);
		delete Current;
		if (LocalSquit)
			ServerInstance->SNO->WriteToSnoMask('l', "Netsplit complete, lost \002%d\002 user%s on \002%d\002 server%s.", num_lost_users, num_lost_users != 1 ? "s" : "", num_lost_servers, num_lost_servers != 1 ? "s" : "");
		else
			ServerInstance->SNO->WriteToSnoMask('L', "Netsplit complete, lost \002%d\002 user%s on \002%d\002 server%s.", num_lost_users, num_lost_users != 1 ? "s" : "", num_lost_servers, num_lost_servers != 1 ? "s" : "");
	}
	else
		ServerInstance->Logs->Log("m_spanningtree",DEFAULT,"Squit from unknown server");
}

/** This function is called when we receive data from a remote
 * server.
 */
void TreeSocket::OnDataReady()
{
	Utils->Creator->loopCall = true;
	/* While there is at least one new line in the buffer,
	 * do something useful (we hope!) with it.
	 */
	while (recvq.find("\n") != std::string::npos)
	{
		std::string ret = recvq.substr(0,recvq.find("\n")-1);
		recvq = recvq.substr(recvq.find("\n")+1,recvq.length()-recvq.find("\n"));
		/* Use rfind here not find, as theres more
		 * chance of the \r being near the end of the
		 * string, not the start.
		 */
		if (ret.find("\r") != std::string::npos)
			ret = recvq.substr(0,recvq.find("\r")-1);
		ProcessLine(ret);
	}
	Utils->Creator->loopCall = false;
}
