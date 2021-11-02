/**
 * vim: set ts=4 :
 * =============================================================================
 * SourceMod Late Downloads Extension
 * Copyright (C) 2004-2008 AlliedModders LLC.  All rights reserved.
 * =============================================================================
 *
 * This program is free software; you can redistribute it and/or modify it under
 * the terms of the GNU General Public License, version 3.0, as published by the
 * Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
 * details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * As a special exception, AlliedModders LLC gives you permission to link the
 * code of this program (as well as its derivative works) to "Half-Life 2," the
 * "Source Engine," the "SourcePawn JIT," and any Game MODs that run on software
 * by the Valve Corporation.  You must obey the GNU General Public License in
 * all respects for all other code used.  Additionally, AlliedModders LLC grants
 * this exception to all derivative works.  AlliedModders LLC defines further
 * exceptions, found in LICENSE.txt (as of this writing, version JULY-31-2007),
 * or <http://www.sourcemod.net/license.php>.
 *
 * Version: $Id$
 */

#include "extension.h"

#include <networkstringtabledefs.h>
#include <filesystem.h>
#include <inetchannel.h>

//#include <utlvector.h>
//#include <UtlStringMap.h>
//#include <utlstring.h>

//#include <IGameConfigs.h>
#include <IBinTools.h>
#include <sm_argbuffer.h>
//#include "game/server/iplayerinfo.h"
//#include <globalvars_base.h>
//#include <edict.h>

CExtension g_Extension;
SMEXT_LINK(&g_Extension);

INetworkStringTableContainer * g_pNSTC = NULL;
INetworkStringTable *g_pDownloadTable = NULL;
IBaseFileSystem * g_pBaseFileSystem = NULL;
IServerPluginHelpers * g_pPluginHelpers = NULL;

IGameConfig *g_pGameConf = NULL;
IBinTools *g_pBinTools = NULL;

extern IGameConfigManager *gameconfs;

ConVar g_cvFileTimeOut("latedl_filetimeout", "10.0", FCVAR_NONE, "Attempt to track LateDL files for this many seconds before giving up.");
ConVar g_cvFileSizeCheckRate("latedl_filesizecheckrate", "7", FCVAR_NONE, "Only process LateDL files every X+1 frames. Anything lower than 0 checks every frame.");

int g_TransferID = 1;
int g_CurrentFrame = 0; // TODO: Figure out how to access the real frame count

void *g_WaitingListAddr;
bool g_bUseFallbackMethod = false;

struct ActiveDownloadClient {
	int id;
	int transferId;
	float timeStarted;
};

struct ActiveDownload {
	CUtlString filename;
	CUtlVector<ActiveDownloadClient> clients;
	bool ignoreTimeout;
};

CUtlVector<ActiveDownload> g_ActiveDownloads;

volatile const char * g_pFlaggedFile = NULL;

SH_DECL_HOOK1_void(IServerGameDLL, GameFrame, SH_NOATTRIB, 0, bool);
SH_DECL_HOOK2(IBaseFileSystem, Size, SH_NOATTRIB, 0, unsigned int, const char *, const char *);

int g_GameFrameHookId = 0;
int g_SizeHookId = 0;

IForward *g_pOnDownloadSuccess = NULL;
IForward *g_pOnDownloadFailure = NULL;

bool CExtension::SDK_OnMetamodLoad(ISmmAPI *ismm, char *error, size_t maxlen, bool late) {
	GET_V_IFACE_ANY(GetEngineFactory, g_pNSTC, INetworkStringTableContainer, INTERFACENAME_NETWORKSTRINGTABLESERVER);
	if (!g_pNSTC) {
		snprintf(error, maxlen, "Couldn't get INetworkStringTableContainer!");
		return false;
	}
	GET_V_IFACE_ANY(GetFileSystemFactory, g_pBaseFileSystem, IBaseFileSystem, BASEFILESYSTEM_INTERFACE_VERSION);
	if (!g_pBaseFileSystem) {
		snprintf(error, maxlen, "Couldn't get IBaseFileSystem!");
		return false;
	}
	GET_V_IFACE_ANY(GetEngineFactory, g_pCVar, ICvar, CVAR_INTERFACE_VERSION);
	if (!g_pCVar) {
		snprintf(error, maxlen, "Couldn't get ICvar!");
		return false;
	}
	GET_V_IFACE_ANY(GetEngineFactory, g_pPluginHelpers, IServerPluginHelpers, INTERFACEVERSION_ISERVERPLUGINHELPERS);
	if (!g_pPluginHelpers) {
		snprintf(error, maxlen, "Couldn't get IServerPluginHelpers!");
		return false;
	}

	IServerPluginCallbacks * serverPluginCallbacks = g_SMAPI->GetVSPInfo(NULL);
	if (!serverPluginCallbacks) {
		snprintf(error, maxlen, "Couldn't get IServerPluginCallbacks!");
		return false;
	}

	g_GameFrameHookId = SH_ADD_VPHOOK(IServerGameDLL, GameFrame, gamedll, SH_MEMBER(this, &CExtension::OnGameFrame), false);
	g_SizeHookId = SH_ADD_VPHOOK(IBaseFileSystem, Size, g_pBaseFileSystem, SH_MEMBER(this, &CExtension::OnSize), false);
	
	ConVar_Register(0, this);
	return true;
}

bool CExtension::SDK_OnLoad(char *error, size_t maxlen, bool late) {

	char conf_error[255] = "";
	
	if (!gameconfs->LoadGameConfigFile("latedl.gamedata", &g_pGameConf, conf_error, sizeof(conf_error)))
	{
		if (conf_error[0])
		{
			//snprintf(error, maxlen, "Could not read latedl.gamedata.txt: %s\n", conf_error);
			smutils->LogError(myself, "Could not read latedl.gamedata.txt: %s\n", conf_error);
		}
		g_bUseFallbackMethod = true;
	}

	if (!g_pGameConf->GetMemSig("CNetChan::IsFileInWaitingList", &g_WaitingListAddr) || !g_WaitingListAddr)
	{
		//snprintf(error, maxlen, "Could not find signature for CNetChan::IsFileInWaitingList.");
		smutils->LogError(myself, "Could not find signature for CNetChan::IsFileInWaitingList.");
		g_bUseFallbackMethod = true;
	}

	return true;
}

void CExtension::SDK_OnUnload() {
	SH_REMOVE_HOOK_ID(g_GameFrameHookId);
	SH_REMOVE_HOOK_ID(g_SizeHookId);
	forwards->ReleaseForward(g_pOnDownloadSuccess);
	forwards->ReleaseForward(g_pOnDownloadFailure);
}


void OnDownloadSuccess(int iClient, const char * filename) {
	//smutils->LogMessage(myself, "Client %d successfully downloaded file '%s'!", iClient, filename);
	if (!g_pOnDownloadSuccess)
		return;
	g_pOnDownloadSuccess->PushCell(iClient);
	g_pOnDownloadSuccess->PushString(filename);
	g_pOnDownloadSuccess->Execute();
}

void OnDownloadFailure(int iClient, const char * filename) {
	smutils->LogMessage(myself, "Client %d failed to download file '%s'!", iClient, filename);
	if (!g_pOnDownloadFailure)
		return;
	g_pOnDownloadFailure->PushCell(iClient);
	g_pOnDownloadFailure->PushString(filename);
	g_pOnDownloadFailure->Execute();
}

bool IsFileInWaitingList(INetChannel *iChannel, const char* filename)
{
	static ICallWrapper *pWrapper = NULL;

	// CNetChan::IsFileInWaitingList(const char* filename)
	if (!pWrapper)
	{
		PassInfo pass[1];
		pass[0].flags = PASSFLAG_BYVAL;
		pass[0].size = sizeof(filename);
		pass[0].type = PassType_Basic;
		PassInfo ret;
		ret.flags = PASSFLAG_BYVAL;
		ret.size = sizeof(bool);
		ret.type = PassType_Basic;
		pWrapper = g_pBinTools->CreateCall(g_WaitingListAddr, CallConv_ThisCall, &ret, pass, 1);
	}

	ArgBuffer<INetChannel*, const char*> vstk(iChannel, filename);

	bool returnBuffer;
	pWrapper->Execute(vstk, &returnBuffer);
	return returnBuffer;
}

void CExtension::OnGameFrame(bool simulating) {
	g_CurrentFrame++;
	float now = Plat_FloatTime();
	
	FOR_EACH_VEC(g_ActiveDownloads, dit) {
		ActiveDownload& activeDownload = g_ActiveDownloads[dit];
		const char * filename = activeDownload.filename;
		FOR_EACH_VEC(activeDownload.clients, cit) {
			int iClient = activeDownload.clients[cit].id;
			
			// Optimization test - don't try to send a file every frame.
			if (g_cvFileSizeCheckRate.GetInt() > 0 && g_CurrentFrame % g_cvFileSizeCheckRate.GetInt() != iClient % g_cvFileSizeCheckRate.GetInt())
				continue;
			
			INetChannel * chan = (INetChannel*)engine->GetPlayerNetInfo(iClient);
			bool resendSuccess;

			if (!chan) {
				smutils->LogError(myself, "Lost client %d when sending file '%s'!", iClient, filename);
				OnDownloadFailure(iClient, filename);
				goto deactivate;
			}
			
			if (!activeDownload.ignoreTimeout && now - activeDownload.clients[cit].timeStarted > g_cvFileTimeOut.GetFloat()) {
				smutils->LogMessage(myself, "Client %s timed out receiving file %s (%d)", chan->GetName(), filename, activeDownload.clients[cit].transferId);
				OnDownloadFailure(iClient, filename);
				goto deactivate;
			}
			
			if (g_bUseFallbackMethod) {

				g_pFlaggedFile = filename;
			
#ifdef DEMO_AWARE
				resendSuccess = chan->SendFile(filename, activeDownload.clients[cit].transferId, false);
#else
				resendSuccess = chan->SendFile(filename, activeDownload.clients[cit].transferId);
#endif

				if (!resendSuccess) {
					smutils->LogError(myself, "Failed to track progress of sending file '%s' to client %d ('%s', %s)!", filename, iClient, chan->GetName(), chan->GetAddress());
					OnDownloadFailure(iClient, filename);
					goto deactivate;
				}
			
				if (g_pFlaggedFile != NULL) {
					g_pFlaggedFile = NULL;
					continue;
				}
			} else {
				bool wl = IsFileInWaitingList(chan, filename);
				smutils->LogMessage(myself, "IsFileInWaitingList: %d %s (%d)", iClient, filename, wl);
				if (wl) {
					continue;
				}
			}
			
			OnDownloadSuccess(iClient, filename);
			goto deactivate;
			
		deactivate:
			activeDownload.clients.FastRemove(cit);
			//reset iterator
			cit--;
		}

		if (activeDownload.clients.Count() == 0) {
			OnDownloadSuccess(0, filename);
			g_ActiveDownloads.FastRemove(dit);
			//reset iterator
			dit--;
		}
	}
}


/*
To clarify what is going on:
Inside the INetChannel::Sendfile() function, several calls to other functions are made.
The one we're insterested in checks wheter the file we're trying to send is already in the send queue.
If that is the case, the function returns True right away.
If not, the function proceeds to the file sending procedure.

If the file was not in the queue, it must've been successfully delivered to the client.

We can tell that by listening on one of the subsequent calls.
One of these calls gets the size of the file we're trying to send.
If this function gets called with our file in parameter, we know that the client have already received the file.

At this moment, we could for example claim that the file doesn't exist.
That would prevent the file from being re-send to the client, but would cause an error message in the server log.

We could also do nothing at all, letting the client re-download the file.
The client would then reject the file and spew an error message in the client log.

Or we could say that the file has size of 0 bytes. The client would re-download these 0 bytes pretty quickly.

This saves a lot of bandwidth and miraculously doesn't cause any kind of error anywhere.
*/

unsigned int CExtension::OnSize(const char *pFileName, const char *pPathID) {
	if (pFileName != NULL && g_pFlaggedFile == pFileName) {
		g_pFlaggedFile = NULL;
		RETURN_META_VALUE(MRES_SUPERCEDE, 0);
	}
	RETURN_META_VALUE(MRES_IGNORED, 0);
}

bool ReloadDownloadTable() {
	g_pDownloadTable = g_pNSTC->FindTable("downloadables");
	return g_pDownloadTable != NULL;
}

void CExtension::OnCoreMapStart(edict_t *pEdictList, int edictCount, int clientMax) {
	if (!ReloadDownloadTable())
		smutils->LogError(myself, "Couldn't load download table!");
	g_ActiveDownloads.RemoveAll();
	float zero = 0;
}

int AddStaticDownloads(CUtlVector<const char*> const & filenames, CUtlVector<const char *> * addedFiles) {
	bool lock = engine->LockNetworkStringTables(true);

	int added = 0;

	FOR_EACH_VEC(filenames, fit) {
		const char * filename = filenames[fit];
		if (g_pDownloadTable->FindStringIndex(filename) != INVALID_STRING_INDEX) {
			OnDownloadSuccess(-1, filename);
			continue;
		}
		if (g_pDownloadTable->AddString(true, filename) == INVALID_STRING_INDEX) {
			smutils->LogError(myself, "Couldn't add file '%s' to download table!", filename);
			OnDownloadFailure(0, filename);
			continue;
		}
		if(addedFiles)
			addedFiles->AddToTail(filename);
		added++;
	}

	engine->LockNetworkStringTables(lock);

	return added;
}

int SendFiles(CUtlVector<const char*> const & filenames, int targetClient = 0, bool ignoreTimeout = false) {
	int firstClient = targetClient > 0 ? targetClient : 1;
	int lastClient = targetClient > 0 ? targetClient : playerhelpers->GetMaxClients();

	int sent = 0;
	FOR_EACH_VEC(filenames, fit) {
		const char * filename = filenames[fit];
		bool failed = false;
		
		ActiveDownload& activeDownload = g_ActiveDownloads[g_ActiveDownloads.AddToTail()];
		activeDownload.filename = filename;
		activeDownload.ignoreTimeout = ignoreTimeout;

		for (int iClient = firstClient; iClient <= lastClient; iClient++)
		{
			INetChannel * chan = (INetChannel*)engine->GetPlayerNetInfo(iClient);
			if (!chan) {
				//if there was a target specified, consider this a failure
				if (targetClient > 0)
					failed = true;
				continue;
			}
#ifdef DEMO_AWARE
			if (chan->SendFile(filename, g_TransferID, false)) {
#else
			if (chan->SendFile(filename, g_TransferID)) {
#endif
				ActiveDownloadClient& client = activeDownload.clients[activeDownload.clients.AddToTail()];
				
				client.id = iClient;
				client.transferId = g_TransferID++;
				client.timeStarted = Plat_FloatTime();
			}
			else {
				failed = true;
			}
		}
		if (failed) {
			if (activeDownload.clients.Count() > 0) {
				smutils->LogError(myself, "This shouldn't have happened! The file %d '%s' have been succesfully sent to some clients, but not to the others. Please inform the author of this extension that this happened!", g_TransferID, filename);
				//provide some info for unfortunate future me
				for (int iClient = firstClient; iClient <= lastClient; iClient++) {
					INetChannel * chan = (INetChannel*)engine->GetPlayerNetInfo(iClient);
					if (!chan)
						continue;
					//this is probably slow
					FOR_EACH_VEC(activeDownload.clients, cit) {
						if (activeDownload.clients[cit].id == iClient) {
							smutils->LogError(myself, "Additional info: Good client %d ('%s', %s)!", iClient, chan->GetName(), chan->GetAddress());
						}
						else {
							smutils->LogError(myself, "Additional info: Bad client %d ('%s', %s)!", iClient, chan->GetName(), chan->GetAddress());
							OnDownloadFailure(iClient, filename);
						}
					}
				}
				sent++;
			}
			else {
				smutils->LogError(myself, "Failed to send file %d '%s'!", g_TransferID, filename);
				OnDownloadFailure(targetClient, filename);
				g_ActiveDownloads.FastRemove(g_ActiveDownloads.Count() - 1);
			}
		}
		else{
			sent++;
		}
	}

	return sent;
}



cell_t AddLateDownloads(IPluginContext *pContext, const cell_t *params) {
	int argc = params[0];
	if (argc != 5)
		return 0;
	cell_t * fileArray = NULL;
	pContext->LocalToPhysAddr(params[1], &fileArray);

	if (fileArray == NULL)
		return 0;

	cell_t numFiles = params[2];
	CUtlVector<const char *> filenames(0, numFiles);

	for (int i = 0; i < numFiles; i++) {
		//Pawn arrays are weird!
		const char * str = (char*)(&fileArray[i]) + fileArray[i];
		filenames.AddToTail(str);
	}

	bool addToDownloadsTable = !!params[3];
	int iClient = params[4];
	bool ignoreTimeout = !!params[5];
	
	CUtlVector<const char *> addedFiles(0, 0);
	CUtlVector<const char *> * addedFilesPtr = &filenames;
	if (addToDownloadsTable) {
		addedFiles.EnsureCapacity(numFiles);
		addedFilesPtr = &addedFiles;
		int added = AddStaticDownloads(filenames, addedFilesPtr);
		if (added == 0)
			return 0;
	}

	int sent = SendFiles(*addedFilesPtr, iClient, ignoreTimeout);
	return sent;
}

cell_t AddLateDownload(IPluginContext *pContext, const cell_t *params) {
	int argc = params[0];
	if (argc != 4)
		return 0;

	char * str;
	CUtlVector<const char *> filenames(0, 1);

	pContext->LocalToString(params[1], &str);
	filenames.AddToTail(str);

	bool addToDownloadsTable = !!params[2];
	int iClient = params[3];
	bool ignoreTimeout = !!params[4];
	
	if (addToDownloadsTable) {
		int added = AddStaticDownloads(filenames, NULL);
		if (added == 0)
			return 0;
	}
	int sent = SendFiles(filenames, iClient, ignoreTimeout);
	return sent;
}

const sp_nativeinfo_t g_Natives[] =
{
	{ "AddLateDownloads", AddLateDownloads },
	{ "AddLateDownload", AddLateDownload },
	{ NULL, NULL },
};

void CExtension::SDK_OnAllLoaded() {
	
	SM_GET_LATE_IFACE(BINTOOLS, g_pBinTools);
	
	sharesys->AddNatives(myself, g_Natives);
	g_pOnDownloadSuccess = forwards->CreateForward("OnDownloadSuccess", ET_Ignore, 2, NULL, Param_Cell, Param_String);
	g_pOnDownloadFailure = forwards->CreateForward("OnDownloadFailure", ET_Ignore, 2, NULL, Param_Cell, Param_String);
	playerhelpers->AddClientListener(this);
}
