#include <filesystem>
#include <fstream>
#include <iostream>
#include "ttddbg_debugger_manager.hh"
#include "ttddbg_strings.hh"
#include <idp.hpp>
#include <ida.hpp>
#include <segment.hpp>

namespace ttddbg
{
	/**********************************************************************/
	bool DebuggerManager::isTargetModule(const TTD::TTD_Replay_Module& module)
	{
		if (m_targetImagePath.string() == Strings::to_string(module.path))
		{
			return true;
		}
		else if (m_targetImagePath.filename() == Strings::find_module_name(module.path))
		{
			return true;
		}
		else
		{
			return false;
		}
	}

	/**********************************************************************/
	DebuggerManager::DebuggerManager(std::shared_ptr<ttddbg::Logger> logger)
		: m_logger(logger), m_isForward { true }, m_resumeMode { resume_mode_t::RESMOD_NONE }, m_positionChooser(new PositionChooser()), m_nextPosition{0}
	{
	}

	/**********************************************************************/
	ssize_t DebuggerManager::onInit(std::string& hostname, int portNumber, std::string& password, qstring* errBuf)
	{
		return DRC_OK;
	}

	/**********************************************************************/
	ssize_t DebuggerManager::onGetProcess(procinfo_vec_t* infos, qstring* errBuf)
	{
		process_info_t info;
		info.name = "test";
		info.pid = 1234;
		infos->push_back(info);
		return DRC_OK;
	}

	/**********************************************************************/
	ssize_t DebuggerManager::onStartProcess(const char* path, const char* args, const char* startdir, uint32 dbg_proc_flags, const char* input_path, uint32 input_file_crc32, qstring* errbuf)
	{	
		m_isForward = true;

		// check if the file exist
		if (!std::filesystem::exists(path))
		{
			m_logger->error("unable to find trace file : ", path);
			return DRC_FAILED;
		}

		std::ifstream traceFile(path, std::ios::out | std::ios::binary);
		if (!traceFile.is_open())
		{
			m_logger->error("unable to open the trace : ", path);
			return DRC_FAILED;
		}

		std::vector<char> magic(6);
		traceFile.read(magic.data(), magic.size());
		traceFile.close();

		if (magic != std::vector<char>({ 'T', 'T', 'D', 'L', 'o', 'g'}))
		{
			m_logger->error("invalid trace file (wrong magic) : ", path);
			return DRC_FAILED;
		}

		// Initialize engine
		if (!m_engine.Initialize(Strings::to_wstring(path).c_str()))
		{
			m_logger->error("unable to load the trace : ", path);
			return DRC_FAILED;
		}

		// init step mode
		m_resumeMode = resume_mode_t::RESMOD_NONE;

		m_cursor = std::make_shared<TTD::Cursor>(m_engine.NewCursor());
		m_positionChooser->setCursor(m_cursor);

		// Populate position chooser (timeline)
		populatePositionChooser();
		
		// Init cursor at the first position
		m_cursor->SetPosition(m_engine.GetFirstPosition());

		m_events.addProcessStartEvent(
			1234,
			m_cursor->GetThreadInfo()->threadid, 
			Strings::to_string(m_engine.GetModuleList()[0].path), 
			m_engine.GetModuleList()[0].base_addr,
			isTargetModule(m_engine.GetModuleList()[0]) ? m_engine.GetModuleList()[0].base_addr : BADADDR,
			m_engine.GetModuleList()[0].imageSize
		);
		
		for (int i = 1; i < m_cursor->GetThreadCount(); i++)
		{
			auto threadId = m_cursor->GetThreadList()[i].info->threadid;
			m_events.addThreadStartEvent(1234, threadId);
		}

		for (int i = 1; i < m_cursor->GetModuleCount(); i++)
		{
			auto moduleInfo = m_cursor->GetModuleList()[i];
			m_events.addLibLoadEvent(
				Strings::to_string(moduleInfo.module->path), 
				moduleInfo.module->base_addr,
				isTargetModule(m_engine.GetModuleList()[0]) ? m_engine.GetModuleList()[0].base_addr : BADADDR,
				moduleInfo.module->imageSize
			);
		}

		m_events.addBreakPointEvent(
			1234,
			m_cursor->GetThreadInfo()->threadid,
			m_cursor->GetProgramCounter()
		);
		
		return DRC_OK;
	}

	/**********************************************************************/
	ssize_t DebuggerManager::onGetDebappAttrs(debapp_attrs_t* attrs)
	{
		return DRC_OK;
	}

	/**********************************************************************/
	ssize_t DebuggerManager::onGetDebugEvent(gdecode_t* code, debug_event_t* event, int timeout_ms)
	{
		if (!m_events.isEmpty())
		{
			*code = GDE_ONE_EVENT;
			*event = m_events.popEvent();
		}
		else
		{
			*code = GDE_NO_EVENT;
		}
		return DRC_OK;
	}

	/**********************************************************************/
	ssize_t DebuggerManager::onGetMemoryInfo(meminfo_vec_t* infos, qstring* errbuf)
	{
		memory_info_t other;
		other.start_ea = 0;
		other.end_ea = (ea_t)0x7FFFFFFFFFFF; // Userland process on windows
		other.bitness = 2;
		infos->push_back(other);
		return DRC_OK;
	}

	/**********************************************************************/
	ssize_t DebuggerManager::onReadMemory(size_t* nbytes, ea_t ea, void* buffer, size_t size, qstring* errbuf)
	{
		auto memory = m_cursor->QueryMemoryBuffer(ea, size);
		*nbytes = memory->size;
		if (memory->size > 0)
		{
			memcpy(buffer, memory->data, size);
		}
		free(memory->data);
		return DRC_OK;
	}

	/**********************************************************************/
	ssize_t DebuggerManager::onRebaseIfRequiredTo(ea_t newBase)
	{
		rebase_program(newBase - get_imagebase(), MSF_FIXONCE);
		return DRC_OK;
	}

	/**********************************************************************/
	ssize_t DebuggerManager::onResume(debug_event_t* event)
	{
		if (event->eid() == event_id_t::BREAKPOINT || event->eid() == event_id_t::STEP)
		{
			if (m_nextPosition.Major != 0 || m_nextPosition.Minor != 0) {
				// Special case: instead of stepping or resuming, if there is a "next position" saved,
				// go to this position instead
				m_logger->info("special case: next position: ", m_nextPosition.Major, " ", m_nextPosition.Minor);
				this->applyCursor(0, m_nextPosition);
				m_events.addBreakPointEvent(1234, m_cursor->GetThreadInfo()[0].threadid, m_cursor->GetProgramCounter());
				m_nextPosition = { 0 };
				return DRC_OK;
			}

			switch (m_resumeMode)
			{
			case resume_mode_t::RESMOD_NONE:
			{
				this->applyCursor(-1);
				m_events.addBreakPointEvent(1234, m_cursor->GetThreadInfo()[0].threadid, m_cursor->GetProgramCounter());
				break;
			}
			case resume_mode_t::RESMOD_INTO:
			{
				this->applyCursor(1);
				m_events.addStepEvent(1234, m_cursor->GetThreadInfo()[0].threadid);
				break;
			}
			default:
				m_logger->info("unsupported resume mode ", (int)m_resumeMode);
				break;
			}
			m_resumeMode = resume_mode_t::RESMOD_NONE;
		}
		return DRC_OK;
	}

	/**********************************************************************/
	ssize_t DebuggerManager::onReadRegisters(thid_t tid, int clsmask, regval_t* values, qstring* errbuf)
	{
		auto threadInfo = m_cursor->GetCrossPlatformContext(tid);

		values[0].ival = threadInfo->rax;
		values[1].ival = threadInfo->rcx;
		values[2].ival = threadInfo->rdx;
		values[3].ival = threadInfo->rbx;
		values[4].ival = threadInfo->rsp;
		values[5].ival = threadInfo->rbp;
		values[6].ival = threadInfo->rsi;
		values[7].ival = threadInfo->rdi;
		values[8].ival = threadInfo->r8;
		values[9].ival = threadInfo->r9;
		values[10].ival = threadInfo->r10;
		values[11].ival = threadInfo->r11;
		values[12].ival = threadInfo->r12;
		values[13].ival = threadInfo->r13;
		values[14].ival = threadInfo->r14;
		values[15].ival = threadInfo->r15;
		values[16].ival = threadInfo->rip;
		values[17].ival = threadInfo->efl;
		
		return DRC_OK;
	}

	/**********************************************************************/
	ssize_t DebuggerManager::onSuspended(bool dllsAdded, thread_name_vec_t* thrNames)
	{
		return DRC_OK;
	}

	/**********************************************************************/
	ssize_t DebuggerManager::onExitProcess(qstring* errbuf)
	{
		m_events.addProcessExitEvent(1234);
		return DRC_OK;
	}

	/**********************************************************************/
	ssize_t DebuggerManager::onGetSrcinfoPath(qstring* path, ea_t base)
	{
		for (int i = 0; i < m_cursor->GetModuleCount(); i++)
		{
			auto module = m_cursor->GetModuleList()[i].module;
			if (module->base_addr == base)
			{
				*path = Strings::to_string(module->path).c_str();
				break;
			}
		}
		return DRC_OK;
	}

	/**********************************************************************/
	ssize_t DebuggerManager::onUpdateBpts(int* nbpts, update_bpt_info_t* bpts, int nadd, int ndel, qstring* errbuf)
	{
		int i = 0;
		*nbpts = 0;
		for (; i < nadd; i++)
		{
			TTD::TTD_Replay_MemoryWatchpointData data;
			data.addr = bpts[i].ea;
			data.size = 8;
			data.flags = TTD::BP_FLAGS::EXEC;
			m_cursor->AddMemoryWatchpoint(&data);
			(*nbpts)++;
		}

		for (; i < ndel; i++)
		{
			TTD::TTD_Replay_MemoryWatchpointData data;
			data.addr = bpts[i].ea;
			data.size = 8;
			data.flags = TTD::BP_FLAGS::EXEC;
			m_cursor->RemoveMemoryWatchpoint(&data);
			(*nbpts)++;
		}
		return DRC_OK;
	}

	/**********************************************************************/
	ssize_t DebuggerManager::onSetResumeMode(thid_t tid, resume_mode_t resmod)
	{
		m_resumeMode = resmod;
		return DRC_OK;
	}

	/**********************************************************************/
	ssize_t DebuggerManager::onUpdateCallStack(thid_t tid, call_stack_t* trace)
	{
		return DRC_NONE;
	}

	/**********************************************************************/
	void DebuggerManager::applyCursor(int steps, TTD::Position newPos) {
		if (steps != 0)
			moveCursorSteps(steps);
		else
			moveCursorPosition(newPos);
	}

	void DebuggerManager::moveCursorSteps(int steps)
	{
		// compute current list of thread
		std::set<uint32_t> threadBefore = getCursorThreads();
		std::set<TTD::TTD_Replay_Module*> moduleBefore = getCursorModules();

		TTD::TTD_Replay_ICursorView_ReplayResult replayrez;
		
		if (m_isForward)
		{
			m_cursor->ReplayForward(&replayrez, m_engine.GetLastPosition(), steps);
		}
		else
		{
			m_cursor->ReplayBackward(&replayrez, m_engine.GetFirstPosition(), steps);
		}

		std::set<uint32_t> threadAfter = getCursorThreads();
		std::set<TTD::TTD_Replay_Module*> moduleAfter = getCursorModules();

		applyDifferences(threadBefore, threadAfter, moduleBefore, moduleAfter);
	}

	void DebuggerManager::moveCursorPosition(TTD::Position newPos) {
		std::set<uint32_t> threadBefore = getCursorThreads();
		std::set<TTD::TTD_Replay_Module*> moduleBefore = getCursorModules();

		TTD::TTD_Replay_ICursorView_ReplayResult replayrez;

		bool forward = true;
		TTD::Position curPos = *m_cursor->GetPosition();
		if (newPos.Major < curPos.Major) {
			forward = false;
		}
		else if (newPos.Major == curPos.Major && newPos.Minor < curPos.Minor) {
			forward = false;
		}

		if (forward) {
			m_cursor->ReplayForward(&replayrez, &newPos, -1);
		}
		else {
			m_cursor->ReplayBackward(&replayrez, &newPos, -1);
		}

		std::set<uint32_t> threadAfter = getCursorThreads();
		std::set<TTD::TTD_Replay_Module*> moduleAfter = getCursorModules();

		applyDifferences(threadBefore, threadAfter, moduleBefore, moduleAfter);
	}

	std::set<uint32_t> DebuggerManager::getCursorThreads() {
		std::set<uint32_t> threads;
		for (int i = 0; i < m_cursor->GetThreadCount(); i++)
		{
			threads.insert(m_cursor->GetThreadList()[i].info->threadid);
		}
		return threads;
	}

	std::set<TTD::TTD_Replay_Module*> DebuggerManager::getCursorModules() {
		std::set<TTD::TTD_Replay_Module*> modules;
		for (int i = 0; i < m_cursor->GetModuleCount(); i++)
		{
			modules.insert(m_cursor->GetModuleList()[i].module);
		}
		return modules;
	}

	void DebuggerManager::applyDifferences(std::set<uint32_t> threadBefore, std::set<uint32_t> threadAfter, std::set<TTD::TTD_Replay_Module*> moduleBefore, std::set<TTD::TTD_Replay_Module*> moduleAfter) {
		// Check created and exited thread between two state
		std::vector<uint32_t> threadExited, threadStarted;

		std::set_difference(threadBefore.begin(), threadBefore.end(), threadAfter.begin(), threadAfter.end(), std::inserter(threadExited, threadExited.begin()));
		std::set_difference(threadAfter.begin(), threadAfter.end(), threadBefore.begin(), threadBefore.end(), std::inserter(threadStarted, threadStarted.begin()));

		std::for_each(threadExited.begin(), threadExited.end(),
			[this](uint32_t threadId) {
				m_events.addThreadExitEvent(1234, threadId);
			}
		);
		std::for_each(threadStarted.begin(), threadStarted.end(),
			[this](uint32_t threadId) {
				m_events.addThreadStartEvent(1234, threadId);
			}
		);

		// Check loaded and unloaded modules
		std::vector<TTD::TTD_Replay_Module*> moduleUnloaded, moduleLoaded;

		std::set_difference(moduleBefore.begin(), moduleBefore.end(), moduleAfter.begin(), moduleAfter.end(), std::inserter(moduleUnloaded, moduleUnloaded.begin()));
		std::set_difference(moduleAfter.begin(), moduleAfter.end(), moduleBefore.begin(), moduleBefore.end(), std::inserter(moduleLoaded, moduleLoaded.begin()));

		std::for_each(moduleUnloaded.begin(), moduleUnloaded.end(),
			[this](TTD::TTD_Replay_Module* module) {
				m_events.addLibUnloadEvent(
					Strings::to_string(module->path),
					module->base_addr
				);
			}
		);

		std::for_each(moduleLoaded.begin(), moduleLoaded.end(),
			[this](TTD::TTD_Replay_Module* module) {
				m_events.addLibLoadEvent(
					Strings::to_string(module->path),
					module->base_addr,
					isTargetModule(m_engine.GetModuleList()[0]) ? m_engine.GetModuleList()[0].base_addr : BADADDR,
					module->imageSize
				);
			}
		);
	}

	/**********************************************************************/
	void DebuggerManager::switchWay()
	{
		m_isForward = !m_isForward;
	}

	void DebuggerManager::openPositionChooser() {
		if (m_positionChooser != nullptr) {
			m_positionChooser->choose();
		}
	}

	void DebuggerManager::setNextPosition(TTD::Position newPos) {
		m_nextPosition = newPos;
	}

	void DebuggerManager::populatePositionChooser() {
		// TODO: use m_engine methods to add timeline positions for each:
		// - Thread creation / exit
		// - Module load / unload
	}
}