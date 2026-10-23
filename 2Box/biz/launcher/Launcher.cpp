module;
#include <Windows.h>
#include <TlHelp32.h>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cwchar>
#include <thread>
#include <vector>

module Launcher;

import "sys_defs.h";
#ifndef _SYS_DEFS_H_
#pragma message("Just for IntelliSense. You should not see this message!")
import "sys_defs.hpp";
#endif

import MainApp;
import EssentialData;
import Utility.SystemInfo;
import Biz.Core;

namespace
{
	// Cache the logical-processor masks belonging to each physical core.
	// On the Ryzen 7 7700, each entry normally contains the two SMT siblings
	// of one physical core. Keeping both siblings in the mask still confines
	// the game's threads to that one physical core.
	const std::vector<DWORD_PTR>& get_physical_core_masks()
	{
		static const std::vector<DWORD_PTR> masks = []
		{
			std::vector<DWORD_PTR> result;
			DWORD bytes = 0;
			GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &bytes);
			if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || bytes == 0)
			{
				return result;
			}

			std::vector<std::byte> buffer(bytes);
			if (!GetLogicalProcessorInformationEx(
				RelationProcessorCore,
				reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(buffer.data()),
				&bytes))
			{
				return result;
			}

			const auto* current = reinterpret_cast<const SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(buffer.data());
			const auto* end = buffer.data() + bytes;
			while (reinterpret_cast<const std::byte*>(current) < end && current->Size >= sizeof(ULONG) * 2)
			{
				if (current->Relationship == RelationProcessorCore && current->Processor.GroupCount > 0)
				{
					// Ryzen 7 7700 has one processor group. Use the first group mask;
					// do not treat the two SMT siblings as separate physical cores.
					const DWORD_PTR mask = static_cast<DWORD_PTR>(current->Processor.GroupMask[0].Mask);
					if (mask != 0)
					{
						result.push_back(mask);
					}
				}
				if (current->Size == 0)
				{
					break;
				}
				current = reinterpret_cast<const SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(
					reinterpret_cast<const std::byte*>(current) + current->Size);
			}
			return result;
		}();
		return masks;
	}

	// Find the actual target executable started by this 2Box cmd.exe process.
	// The existing Detours injection/start chain is preserved; this only observes
	// the child process and applies affinity after the game process exists.
	HANDLE find_target_child_process(DWORD parentPid, std::wstring_view targetPath)
	{
		const std::wstring expectedPath{std::filesystem::path{targetPath}.native()};
		for (int attempt = 0; attempt < 40; ++attempt)
		{
			HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
			if (snapshot != INVALID_HANDLE_VALUE)
			{
				PROCESSENTRY32W entry{sizeof(entry)};
				if (Process32FirstW(snapshot, &entry))
				{
					do
					{
						if (entry.th32ParentProcessID != parentPid)
						{
							continue;
						}

						HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_SET_INFORMATION, FALSE, entry.th32ProcessID);
						if (!process)
						{
							continue;
						}

						std::vector<wchar_t> imagePath(32768);
						DWORD imagePathLength = static_cast<DWORD>(imagePath.size());
						const bool gotPath = QueryFullProcessImageNameW(process, 0, imagePath.data(), &imagePathLength) != FALSE;
						const bool matches = gotPath && CompareStringOrdinal(
							expectedPath.c_str(), static_cast<int>(expectedPath.size()),
							imagePath.data(), static_cast<int>(imagePathLength), TRUE) == CSTR_EQUAL;
						if (matches)
						{
							CloseHandle(snapshot);
							return process;
						}
						CloseHandle(process);
					} while (Process32NextW(snapshot, &entry));
				}
				CloseHandle(snapshot);
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(50));
		}
		return nullptr;
	}

	void apply_affinity_to_target_process(DWORD launcherPid, std::wstring_view exePath)
	{
		const auto& masks = get_physical_core_masks();
		if (masks.empty())
		{
			OutputDebugStringW(L"2Box: CPU topology unavailable; leaving game affinity unchanged.\n");
			return;
		}

		HANDLE gameProcess = find_target_child_process(launcherPid, exePath);
		if (!gameProcess)
		{
			OutputDebugStringW(L"2Box: target game process was not found; leaving affinity unchanged.\n");
			return;
		}

		static std::atomic_size_t nextCore{0};
		const size_t coreIndex = nextCore.fetch_add(1, std::memory_order_relaxed) % masks.size();
		if (!SetProcessAffinityMask(gameProcess, masks[coreIndex]))
		{
			OutputDebugStringW(L"2Box: SetProcessAffinityMask failed; game started without an affinity change.\n");
		}
		CloseHandle(gameProcess);
	}

	PROCESS_INFORMATION create_and_inject(const biz::Env* env, std::wstring_view exePath, std::wstring_view params)
	{
		PROCESS_INFORMATION procInfo = {nullptr};
		STARTUPINFOW startupInfo = {sizeof(startupInfo)};
		startupInfo.dwFlags = STARTF_USESHOWWINDOW;
		startupInfo.wShowWindow = SW_HIDE;

		namespace fs = std::filesystem;
		const fs::path cmdPath{fs::weakly_canonical(fs::path{sys_info::get_system_dir()} / fs::path{L"cmd.exe"})};
		std::wstring cmdLine = params.empty() ? std::format(LR"(/c start "" "{}")", exePath) : std::format(LR"(/c start "" "{}" {})", exePath, params);
		if (!DetourCreateProcessWithDllExW(cmdPath.c_str(), cmdLine.data(), nullptr, nullptr, 0,
		                                   CREATE_DEFAULT_ERROR_MODE | CREATE_SUSPENDED, nullptr,
		                                   std::filesystem::path{exePath}.parent_path().native().c_str(), &startupInfo, &procInfo,
		                                   env->ensureDllInDeviceAndReturnPath().c_str(), &::CreateProcessW))
		{
			throw std::runtime_error(std::format("CreateProcessW Failed, error code: {}", GetLastError()));
		}
		try
		{
			const std::wstring_view rootPath = app().exeDir();
			const std::uint32_t rootPathCount = static_cast<std::uint32_t>(rootPath.length());
			const std::uint32_t rootPathSize = rootPathCount * sizeof(wchar_t);
			const std::uint32_t paramsSize = sizeof(DetourInjectParams) + rootPathSize;
			std::vector<std::byte> buffer(paramsSize);
			DetourInjectParams* injectParams = reinterpret_cast<DetourInjectParams*>(buffer.data());
			injectParams->version = biz::get_core_data().version;
			injectParams->envFlag = env->getFlag();
			injectParams->envIndex = env->getIndex();
			injectParams->rootPathCount = rootPathCount;
			memcpy(injectParams->rootPath, rootPath.data(), rootPathSize);
			if (!DetourCopyPayloadToProcess(procInfo.hProcess, DETOUR_INJECT_PARAMS_GUID, injectParams, paramsSize))
			{
				throw std::runtime_error(std::format("copy payload failed, error code: {}", GetLastError()));
			}
		}
		catch (...)
		{
			TerminateProcess(procInfo.hProcess, 0);
			CloseHandle(procInfo.hThread);
			CloseHandle(procInfo.hProcess);
			throw;
		}
		return procInfo;
	}
}

namespace biz
{
	void Launcher::run(const std::shared_ptr<Env>& env, std::wstring_view exePath, std::wstring_view params /*= L""*/)
	{
		m_asyncScope.spawn(launch(env, exePath, params));
	}

	void Launcher::runInNewEnv(std::wstring_view exePath, std::wstring_view params /*= L""*/)
	{
		m_asyncScope.spawn(launch(std::shared_ptr<Env>{}, exePath, params));
	}

	coro::LazyTask<void> Launcher::coRun(std::shared_ptr<Env> env, std::wstring_view exePath, std::wstring_view params)
	{
		coro::SharedTask<void> sharedTask = coro::start_and_shared(launchInternal(env, std::wstring{exePath}, std::wstring{params}));
		m_asyncScope.spawn(sharedTask);
		co_await sharedTask;
		co_return;
	}

	coro::LazyTask<void> Launcher::launch(const std::shared_ptr<Env>& env, std::wstring_view exePath, std::wstring_view params) const
	{
		try
		{
			co_await launchInternal(env, std::wstring{exePath}, std::wstring{params});
		}
		catch (const std::exception& e)
		{
			show_utf8_error_message(std::format("启动进程失败：{}", e.what()));
		}
		catch (...)
		{
			show_error_message(L"启动进程失败：发生未知错误");
		}
		co_return;
	}

	coro::LazyTask<void> Launcher::launchInternal(std::shared_ptr<Env> env, std::wstring exePath, std::wstring params) const
	{
		co_await sched::transfer_to(m_execCtx);

		if (!env)
		{
			env = env_mgr().createEnv();
		}
		const PROCESS_INFORMATION procInfo = create_and_inject(env.get(), exePath, params);
		const DWORD launcherPid = procInfo.dwProcessId;
		ResumeThread(procInfo.hThread);
		CloseHandle(procInfo.hThread);
		// Keep the original sandbox/injection launch chain intact. Once cmd.exe
		// starts the requested executable, set affinity on that game process itself.
		try
		{
			apply_affinity_to_target_process(launcherPid, exePath);
		}
		catch (...)
		{
			// Affinity is an optional optimization; never turn its failure into
			// a failed game launch after the original process has already resumed.
			OutputDebugStringW(L"2Box: unexpected CPU-affinity error; game launch continues.\n");
		}
		CloseHandle(procInfo.hProcess);
		co_return;
	}
}
