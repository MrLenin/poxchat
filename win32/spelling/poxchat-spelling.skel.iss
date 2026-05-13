AppName=PoxChat Spelling Dictionaries
AppVerName=PoxChat Spelling Dictionaries r1
AppVersion=1.0
VersionInfoVersion=1.0
OutputBaseFilename=PoxChat Spelling Dictionaries r1
AppPublisher=PoxChat
AppPublisherURL=https://github.com/evilnet/poxchat
AppCopyright=Copyright (C) 1998-2010 Peter Zelezny
AppSupportURL=https://github.com/evilnet/poxchat/issues
AppUpdatesURL=https://github.com/evilnet/poxchat/releases
DefaultDirName={localappdata}\enchant
DefaultGroupName=PoxChat
DisableProgramGroupPage=yes
DisableDirPage=yes
SolidCompression=yes
Compression=lzma2/ultra64
SourceDir=.
OutputDir=.
FlatComponentsList=no
PrivilegesRequired=lowest
ShowComponentSizes=no
CreateUninstallRegKey=no
Uninstallable=no
DirExistsWarning=no
ArchitecturesAllowed=x86 x64

[Files]
Source: "myspell\*"; DestDir: "{app}\myspell"; Flags: createallsubdirs recursesubdirs

[Messages]
BeveledLabel= PoxChat
