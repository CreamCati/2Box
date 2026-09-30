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
	// ============================================================
	// 获取物理 CPU 核心对应的第一个逻辑处理器
	//
	// 例如：
	//
	// 物理核心0 -> CPU 0 + CPU 1
	// 物理核心1 -> CPU 2 + CPU 3
	// 物理核心2 -> CPU 4 + CPU 5
	//
	// 最终返回：
	// [CPU0, CPU2, CPU4, CPU6 ...]
	//
	// 注意：
	// 这里只使用物理核心，不会把 SMT/超线程当成独立核心。
	// ============================================================
	std::vector<DWORD_PTR> get_physical_core_affinity_masks()
	{
		std::vector<DWORD_PTR> result;

		DWORD bufferSize = 0;

		if (!GetLogicalProcessorInformationEx(
			RelationProcessorCore,
			nullptr,
			&bufferSize))
		{
			if (GetLastError() != ERROR_INSUFFICIENT_BUFFER)
			{
				return result;
			}
		}

		if (bufferSize == 0)
		{
			return result;
		}

		std::vector<std::byte> buffer(bufferSize);

		auto* info =
			reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(buffer.data());

		if (!GetLogicalProcessorInformationEx(
			RelationProcessorCore,
			info,
			&bufferSize))
		{
			return result;
		}

		DWORD offset = 0;

		while (offset < bufferSize)
		{
			auto* current =
				reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(
					buffer.data() + offset);

			if (current->Relationship == RelationProcessorCore)
			{
				// 一个物理核心可能对应多个逻辑处理器
				// 这里只取这个物理核心的第一个逻辑处理器。
				for (WORD groupIndex = 0;
				     groupIndex < current->Processor.GroupCount;
				     ++groupIndex)
				{
					const GROUP_AFFINITY& group =
						current->Processor.GroupMask[groupIndex];

					// 当前 2Box 使用的进程通常位于 Group 0。
					// 普通桌面 CPU 基本都会走这里。
					if (group.Group != 0)
					{
						continue;
					}

					const KAFFINITY mask = group.Mask;

					if (mask == 0)
					{
						continue;
					}

					// 取这个物理核心的第一个逻辑处理器。
					const KAFFINITY firstProcessor =
						mask & (~mask + 1);

					result.push_back(
						static_cast<DWORD_PTR>(firstProcessor));
				}
			}

			if (current->Size == 0)
			{
				break;
			}

			offset += current->Size;
		}

		return result;
	}


	// ============================================================
	// 根据环境 index 获取 CPU Affinity
	//
	// env index：
	//
	// 环境0 -> 第1个物理核心
	// 环境1 -> 第2个物理核心
	// 环境2 -> 第3个物理核心
	// ...
	//
	// 如果实例数量超过物理核心数量，则循环使用。
	//
	// 例如 4 核：
	//
	// 环境0 -> Core 0
	// 环境1 -> Core 1
	// 环境2 -> Core 2
	// 环境3 -> Core 3
	// 环境4 -> Core 0
	// 环境5 -> Core 1
	// ============================================================
	DWORD_PTR get_affinity_mask_for_env(std::uint32_t envIndex)
	{
		static const std::vector<DWORD_PTR> physicalCores =
			get_physical_core_affinity_masks();

		if (physicalCores.empty())
		{
			// 极少数异常情况下无法读取 CPU 拓扑。
			// 直接不限制 CPU，避免影响原有启动功能。
			return 0;
		}

		return physicalCores[
			static_cast<std::size_t>(envIndex) % physicalCores.size()
		];
	}


	// ============================================================
	// 创建并注入目标进程
	// ============================================================
	PROCESS_INFORMATION create_and_inject(
		const biz::Env* env,
		std::wstring_view exePath,
		std::wstring_view params)
	{
		PROCESS_INFORMATION procInfo = {nullptr};

		STARTUPINFOW startupInfo = {sizeof(startupInfo)};
		startupInfo.dwFlags = STARTF_USESHOWWINDOW;
		startupInfo.wShowWindow = SW_HIDE;

		namespace fs = std::filesystem;

		const fs::path cmdPath{
			fs::weakly_canonical(
				fs::path{sys_info::get_system_dir()} /
				fs::path{L"cmd.exe"})
		};

		std::wstring cmdLine =
			params.empty()
			? std::format(
				LR"(/c start "" "{}")",
				exePath)
			: std::format(
				LR"(/c start "" "{}" {})",
				exePath,
				params);

		// ========================================================
		// 创建挂起状态的 cmd.exe
		// ========================================================
		if (!DetourCreateProcessWithDllExW(
			cmdPath.c_str(),
			cmdLine.data(),
			nullptr,
			nullptr,
			0,
			CREATE_DEFAULT_ERROR_MODE | CREATE_SUSPENDED,
			nullptr,
			fs::path{exePath}.parent_path().native().c_str(),
			&startupInfo,
			&procInfo,
			env->ensureDllInDeviceAndReturnPath().c_str(),
			&::CreateProcessW))
		{
			throw std::runtime_error(
				std::format(
					"CreateProcessW Failed, error code: {}",
					GetLastError()));
		}

		try
		{
			// ====================================================
			// 写入 2Box 注入参数
			// ====================================================
			const std::wstring_view rootPath = app().exeDir();

			const std::uint32_t rootPathCount =
				static_cast<std::uint32_t>(rootPath.length());

			const std::uint32_t rootPathSize =
				rootPathCount * sizeof(wchar_t);

			const std::uint32_t paramsSize =
				sizeof(DetourInjectParams) + rootPathSize;

			std::vector<std::byte> buffer(paramsSize);

			DetourInjectParams* injectParams =
				reinterpret_cast<DetourInjectParams*>(buffer.data());

			injectParams->version = biz::get_core_data().version;
			injectParams->envFlag = env->getFlag();
			injectParams->envIndex = env->getIndex();
			injectParams->rootPathCount = rootPathCount;

			memcpy(
				injectParams->rootPath,
				rootPath.data(),
				rootPathSize);

			if (!DetourCopyPayloadToProcess(
				procInfo.hProcess,
				DETOUR_INJECT_PARAMS_GUID,
				injectParams,
				paramsSize))
			{
				throw std::runtime_error(
					std::format(
						"copy payload failed, error code: {}",
						GetLastError()));
			}


			// ====================================================
			// CPU 自动分配
			//
			// 此时 cmd.exe 仍然处于 CREATE_SUSPENDED 状态。
			//
			// 给 cmd.exe 设置 Affinity 后：
			//
			// cmd.exe
			//    ↓
			// start.exe / 目标程序
			//
			// 子进程会继承这个 CPU Affinity。
			// ====================================================
			const DWORD_PTR affinityMask =
				get_affinity_mask_for_env(env->getIndex());

			if (affinityMask != 0)
			{
				if (!SetProcessAffinityMask(
					procInfo.hProcess,
					affinityMask))
				{
					throw std::runtime_error(
						std::format(
							"SetProcessAffinityMask failed, error code: {}",
							GetLastError()));
				}
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
	void Launcher::run(
		const std::shared_ptr<Env>& env,
		std::wstring_view exePath,
		std::wstring_view params /*= L""*/)
	{
		m_asyncScope.spawn(
			launch(env, exePath, params));
	}


	void Launcher::runInNewEnv(
		std::wstring_view exePath,
		std::wstring_view params /*= L""*/)
	{
		m_asyncScope.spawn(
			launch(
				std::shared_ptr<Env>{},
				exePath,
				params));
	}


	coro::LazyTask<void> Launcher::coRun(
		std::shared_ptr<Env> env,
		std::wstring_view exePath,
		std::wstring_view params)
	{
		coro::SharedTask<void> sharedTask =
			coro::start_and_shared(
				launchInternal(
					env,
					std::wstring{exePath},
					std::wstring{params}));

		m_asyncScope.spawn(sharedTask);

		co_await sharedTask;

		co_return;
	}


	coro::LazyTask<void> Launcher::launch(
		const std::shared_ptr<Env>& env,
		std::wstring_view exePath,
		std::wstring_view params) const
	{
		try
		{
			co_await launchInternal(
				env,
				std::wstring{exePath},
				std::wstring{params});
		}
		catch (const std::exception& e)
		{
			show_utf8_error_message(
				std::format(
					"启动进程失败：{}",
					e.what()));
		}
		catch (...)
		{
			show_error_message(
				L"启动进程失败：发生未知错误");
		}

		co_return;
	}


	coro::LazyTask<void> Launcher::launchInternal(
		std::shared_ptr<Env> env,
		std::wstring exePath,
		std::wstring params) const
	{
		co_await sched::transfer_to(m_execCtx);

		if (!env)
		{
			env = env_mgr().createEnv();
		}

		const PROCESS_INFORMATION procInfo =
			create_and_inject(
				env.get(),
				exePath,
				params);

		// CPU Affinity 已经在 ResumeThread 前设置完成。
		ResumeThread(procInfo.hThread);

		CloseHandle(procInfo.hThread);
		CloseHandle(procInfo.hProcess);

		co_return;
	}
}