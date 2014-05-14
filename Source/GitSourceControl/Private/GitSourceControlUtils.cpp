// Copyright (c) 2014 Sebastien Rombauts (sebastien.rombauts@gmail.com)
//
// Distributed under the MIT License (MIT) (See accompanying file LICENSE.txt
// or copy at http://opensource.org/licenses/MIT)

#include "GitSourceControlPrivatePCH.h"
#include "GitSourceControlUtils.h"
#include "GitSourceControlState.h"
#include "GitSourceControlModule.h"
#include "GitSourceControlCommand.h"

namespace GitSourceControlConstants
{
	/** The maximum number of files we submit in a single Git command */
	const int32 MaxFilesPerBatch = 20;
}

namespace GitSourceControlUtils
{

static bool RunCommandInternal(const FString& InGitBinaryPath, const FString& InRepositoryRoot, const FString& InCommand, const TArray<FString>& InParameters, const TArray<FString>& InFiles, FString& OutResults, FString& OutErrors)
{
	int32 ReturnCode = 0;
	FString FullCommand;
	FString LogableCommand; // short version of the command for logging purpose

	if (!InRepositoryRoot.IsEmpty())
	{
		// Specify the working copy (the root) of the git repository (before the command itself)
		FullCommand  = TEXT("--work-tree=\"");
		FullCommand += InRepositoryRoot;
		// and the ".git" subdirectory in it (before the command itself)
		FullCommand += TEXT("\" --git-dir=\"");
		FullCommand += InRepositoryRoot;
		FullCommand += TEXT(".git\" ");
	}
	// then the git command itself ("status", "log", "commit"...)
	LogableCommand += InCommand;

	// Append to the command all parameters, and then finally the files
	for(const auto& Parameter : InParameters)
	{
		LogableCommand += TEXT(" ");
		LogableCommand += Parameter;
	}
	for(const auto& File : InFiles)
	{
		LogableCommand += TEXT(" \"");
		LogableCommand += File;
		LogableCommand += TEXT("\"");
	}
	// Also, Git does not have a "--non-interactive" option, as it auto-detects when there are no connected standard input/output streams

	FullCommand += LogableCommand;

	// @todo: temporary debug logs
	UE_LOG(LogSourceControl, Log, TEXT("RunCommandInternal: Attempting 'git %s'"), *LogableCommand);
	FPlatformProcess::ExecProcess(*InGitBinaryPath, *FullCommand, &ReturnCode, &OutResults, &OutErrors);
	UE_LOG(LogSourceControl, Log, TEXT("RunCommandInternal: ExecProcess ReturnCode=%d OutResults='%s'"), ReturnCode, *OutResults);
	if(!OutErrors.IsEmpty())
	{
		UE_LOG(LogSourceControl, Error, TEXT("RunCommandInternal: ExecProcess ReturnCode=%d OutErrors='%s'"), ReturnCode, *OutErrors);
	}

	return ReturnCode == 0;
}

FString FindGitBinaryPath()
{
	bool bFound = false;

#if PLATFORM_WINDOWS
	// 1) First of all, check for the ThirdParty directory as it may contain a specific version of Git for this plugin to work
	// NOTE using only "git" (or "git.exe") relying on the "PATH" envvar does not always work as expected, depending on the installation:
	// If the PATH is set with "git/cmd" instead of "git/bin",
	// "git.exe" launch "git/cmd/git.exe" that redirect to "git/bin/git.exe" and ExecProcess() is unable to catch its outputs streams.

	// Under Windows, we can use the third party "msysgit PortableGit" https://code.google.com/p/msysgit/downloads/list?can=1&q=PortableGit
	// NOTE: Win32 platform subdirectory as there is no Git 64bit build available
	FString GitBinaryPath(FPaths::EngineDir() / TEXT("Binaries/ThirdParty/git/Win32/bin") / TEXT("git.exe"));
	bFound = CheckGitAvailability(GitBinaryPath);
#endif

	// 2) If Git is not found in ThirdParty directory, look into standard install directory
	if(!bFound)
	{
#if PLATFORM_WINDOWS
		// @todo use the Windows registry to find Git
		GitBinaryPath = TEXT("C:/Program Files (x86)/Git/bin/git.exe");
#else
		GitBinaryPath = TEXT("/usr/bin/git");
#endif
	}

	return GitBinaryPath;
}

bool CheckGitAvailability(const FString& InGitBinaryPath)
{
	bool bGitAvailable = false;

	FString InfoMessages;
	FString ErrorMessages;
	bGitAvailable = RunCommandInternal(InGitBinaryPath, FString(), TEXT("version"), TArray<FString>(), TArray<FString>(), InfoMessages, ErrorMessages);
	if(bGitAvailable)
	{
		if(InfoMessages.Contains("git"))
		{
			bGitAvailable = true;
		}
		else
		{
			bGitAvailable = false;
		}
	}

	// @todo also check Git config user.name & user.email

	return bGitAvailable;
}

bool RunCommand(const FString& InGitBinaryPath, const FString& InRepositoryRoot, const FString& InCommand, const TArray<FString>& InParameters, const TArray<FString>& InFiles, TArray<FString>& OutResults, TArray<FString>& OutErrorMessages)
{
	bool bResult = true;

	if(InFiles.Num() > GitSourceControlConstants::MaxFilesPerBatch)
	{
		// Batch files up so we dont exceed command-line limits
		int32 FileCount = 0;
		while(FileCount < InFiles.Num())
		{
			TArray<FString> FilesInBatch;
			for(int32 FileIndex = 0; FileIndex < InFiles.Num() && FileIndex < GitSourceControlConstants::MaxFilesPerBatch; FileIndex++, FileCount++)
			{
				FilesInBatch.Add(InFiles[FileIndex]);
			}

			FString Results;
			FString Errors;
			bResult &= RunCommandInternal(InGitBinaryPath, InRepositoryRoot, InCommand, InParameters, FilesInBatch, Results, Errors);
			Results.ParseIntoArray(&OutResults, TEXT("\n"), true);
			Errors.ParseIntoArray(&OutErrorMessages, TEXT("\n"), true);
		}
	}
	else
	{
		FString Results;
		FString Errors;
		bResult &= RunCommandInternal(InGitBinaryPath, InRepositoryRoot, InCommand, InParameters, InFiles, Results, Errors);
		Results.ParseIntoArray(&OutResults, TEXT("\n"), true);
		Errors.ParseIntoArray(&OutErrorMessages, TEXT("\n"), true);
	}

	return bResult;
}


void ParseLogResults(const TArray<FString>& InResults, TGitSourceControlHistory& OutHistory)
{
	// @todo Example git log results:
	//
	//commit 2d92a17eb3f5211d1b874a6530a080ba7d5f10bc
	//Author: Sébastien Rombauts <sebastien.rombauts@gmail.com>
	//Date:   Mon May 12 20:05:36 2014 +0200
	//
	//    Fixed some of the bugs with working copy file states
	//
	//     - still a bug present; missing an UpdateStatus after saving a Blueprint!
	//
	//commit 8220e00289679e202603a83c052584ad9185b140
	//Author: Sébastien Rombauts <sebastien.rombauts@gmail.com>
	//Date:   Mon May 12 07:12:00 2014 +0200
	//
	//    Added ExectuteSynchronousCommand needed for the "revert" operation
	//
	for(const auto& Result : InResults)
	{
		// @todo parse git logs
		TSharedRef<FGitSourceControlRevision, ESPMode::ThreadSafe> SourceControlRevision = MakeShareable(new FGitSourceControlRevision);
		SourceControlRevision->Filename = "???";
		SourceControlRevision->Description = Result; // @todo test value
		// @todo Action, Date, UserName, Revision
		OutHistory.Add(SourceControlRevision);
	}
}

/** Match the relative filename of a Git status result with a provided absolute filename */
class FGitStatusFileMatcher
{
public:
	FGitStatusFileMatcher(const FString& InAbsoluteFilename)
		: AbsoluteFilename(InAbsoluteFilename)
	{
	}

	bool Matches(const FString& InResult) const
	{
		// Extract the relative filename from the Git status result
		// @todo this can not work in case of a rename from -> to
		FString RelativeFilename = InResult.RightChop(3);
		return AbsoluteFilename.Contains(RelativeFilename);
	}

private:
	const FString& AbsoluteFilename;
};

/** 
 * Extract and interpret the file state from the given Git status result.
 * @see file:///C:/Program%20Files%20(x86)/Git/doc/git/html/git-status.html
 * ' ' = unmodified
 * 'M' = modified
 * 'A' = added
 * 'D' = deleted
 * 'R' = renamed
 * 'C' = copied
 * 'U' = updated but unmerged
 * '?' = unknown/untracked
 * '!' = ignored
 */
class FGitStatusParser
{
public:
	FGitStatusParser(const FString& InResult)
	{
		// @todo Get the second part of a rename "from -> to"
		//FString Filename = InResult.RightChop(3);

		TCHAR IndexState = InResult[0];
		TCHAR WCopyState = InResult[1];
		if(	  (IndexState == 'U' || WCopyState == 'U')
		   || (IndexState == 'A' && WCopyState == 'A')
		   || (IndexState == 'D' && WCopyState == 'D'))
		{
			// "Unmerged" conflict cases are generally marked with a "U",
			// but there are also the special cases of both "A"dded, or both "D"eleted
			State = EWorkingCopyState::Conflicted;
		}
		else if(IndexState == 'A')
		{
			State = EWorkingCopyState::Added;
		}
		else if(IndexState == 'D')
		{
			State = EWorkingCopyState::Deleted;
		}
		else if(WCopyState == 'D')
		{
			State = EWorkingCopyState::Missing;
		}
		else if(IndexState == 'M' || WCopyState == 'M')
		{
			State = EWorkingCopyState::Modified;
		}
		else if(IndexState == 'R')
		{
			State = EWorkingCopyState::Renamed;
		}
		else if(IndexState == 'C')
		{
			State = EWorkingCopyState::Copied;
		}
		else if(IndexState == '?' || WCopyState == '?')
		{
			State = EWorkingCopyState::NotControlled;
		}
		else if(IndexState == '!' || WCopyState == '!')
		{
			State = EWorkingCopyState::Ignored;
		}
		else
		{
			// Unmodified never yield a status
			State = EWorkingCopyState::Unknown;
		}
	}

	EWorkingCopyState::Type State;
};

void ParseStatusResults(const TArray<FString>& InFiles, const TArray<FString>& InResults, TArray<FGitSourceControlState>& OutStates)
{
	for(const auto& File : InFiles)
	{
		FGitSourceControlState FileState(File);
		FGitStatusFileMatcher FileMatcher(File);
		int32 IdxResult = InResults.FindMatch(FileMatcher);
		if(IdxResult != INDEX_NONE)
		{
			// File found in status results; only the case for "changed" files
			FGitStatusParser StatusParser(InResults[IdxResult]);
			FileState.WorkingCopyState = StatusParser.State;
		}
		else
		{
			// File not found in status
			if(FPaths::FileExists(File))
			{
				// usually means the file is unchanged,
				FileState.WorkingCopyState = EWorkingCopyState::Unchanged;
			}
			else
			{
				// but also the case for newly created content: there is no file on disk until the content is saved for the first time
				FileState.WorkingCopyState = EWorkingCopyState::NotControlled;
			}
		}
		FileState.TimeStamp.Now();
		OutStates.Add(FileState);
	}
}

bool UpdateCachedStates(const TArray<FGitSourceControlState>& InStates)
{
	FGitSourceControlModule& GitSourceControl = FModuleManager::LoadModuleChecked<FGitSourceControlModule>( "GitSourceControl" );
	FGitSourceControlProvider& Provider = GitSourceControl.GetProvider();
	int NbStatesUpdated = 0;

	for(const auto& InState : InStates)
	{
		TSharedRef<FGitSourceControlState, ESPMode::ThreadSafe> State = Provider.GetStateInternal(InState.LocalFilename);
		if(State->WorkingCopyState != InState.WorkingCopyState)
		{
			State->WorkingCopyState = InState.WorkingCopyState;
		//	State->TimeStamp = FDateTime::Now(); // @todo Workaround a bug with the Source Control Module not updating file state after a "Save"
			NbStatesUpdated++;
		}
	}

	return (NbStatesUpdated > 0);
}

}
