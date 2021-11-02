# Late Downloads 2 (Sappy Edition)
## What is this?
This is a [SourceMod](http://www.sourcemod.net/) extension that allows file transfers to players that are already in the game.
Specifically, this is a fork of [jonatan1024's version](https://github.com/jonatan1024/latedl).
## How to build this?
Just as any other [AMBuild](https://wiki.alliedmods.net/AMBuild) project:
1. [Install AMBuild](https://wiki.alliedmods.net/AMBuild#Installation)
2. Download [Half-Life 2 SDK](https://github.com/alliedmodders/hl2sdk), [Metamod:Source](https://github.com/alliedmodders/metamod-source/) and [SourceMod](https://github.com/alliedmodders/sourcemod)
3. `mkdir build && cd build`
4. `python ../configure.py --hl2sdk-root=??? --mms-path=??? --sm-path=??? --sdks=csgo`
5. `ambuild`
## How to use this?
Simply copy the extension binary to the `extensions` folder and the include file into the `scripting/include` folder.

Now just create a new plugin and include [`latedl.inc`](include/latedl.inc).
## Sample script
```pawn
#pragma semicolon 1
#pragma newdecls required

#include <sourcemod>
#include <latedl>

public Plugin myinfo = 
{
	name = "My First Plugin", 
	author = "Me", 
	description = "My first plugin ever", 
	version = "1.0", 
	url = "http://www.sourcemod.net/"
};

public void OnPluginStart()
{
	RegAdminCmd("testdl", Command_TestDL, ADMFLAG_SLAY);
}

public void OnDownloadSuccess(int iClient, char[] filename) {
	if (iClient > 0)
		return;
	PrintToServer("All players successfully downloaded file '%s'!", filename);
}

public Action Command_TestDL(int client, int args)
{
	//create arbitrary file
	int time = GetTime();
	char tstr[64];
	FormatEx(tstr, 64, "%d.txt", time);
	File file = OpenFile(tstr, "wt", true, NULL_STRING);
	WriteFileString(file, tstr, false);
	CloseHandle(file);
	
	//send
	AddLateDownload(tstr);
	return Plugin_Handled;
}
```
## Extension configuration
The extension exposes the following cvars:
* `latedl_filetimeout` (default = 10) - Flag downloads as failed after this many seconds.
* `latedl_filesizecheckrate` (default = 7) - How often to check for file status.

These cvars only come into effect if `CNetChan::IsFileInWaitingList` is not found in the supplied gamedata file.  Checking the file sending queue is much less resource intensive, but requires a bit of memory patching since the function is not publicly exposed.

## Additional information
* This was only tested in TF2 and L4D2, but any modern Source game (OrangeBox+) should be ok.
* This extension (the original version) used to be a part of the [Gorme](https://github.com/jonatan1024/gorme) project.
