
#include "DebugLogger.h"

#include "MsgThread.h"
#include "Manager.h"

#include <unistd.h>
#include <signal.h>

using namespace threading;

namespace threading  {

////// Messages.

// Signals child thread to shutdown operation.
class FinishMessage : public InputMessage<MsgThread>
{
public:
	FinishMessage(MsgThread* thread, double network_time) : InputMessage<MsgThread>("Finish", thread),
		network_time(network_time) { }

	virtual bool Process()	{
		bool result = Object()->OnFinish(network_time);
		Object()->Finished();
		return result;
	}

private:
	double network_time;
};

/// Sends a heartbeat to the child thread.
class HeartbeatMessage : public InputMessage<MsgThread>
{
public:
	HeartbeatMessage(MsgThread* thread, double arg_network_time, double arg_current_time)
		: InputMessage<MsgThread>("Heartbeat", thread)
		{ network_time = arg_network_time; current_time = arg_current_time; }

	virtual bool Process()	{
		Object()->HeartbeatInChild();
		return Object()->OnHeartbeat(network_time, current_time);
	}

private:
	double network_time;
	double current_time;
};

// A message from the child to be passed on to the Reporter.
class ReporterMessage : public OutputMessage<MsgThread>
{
public:
	enum Type {
		INFO, WARNING, ERROR, FATAL_ERROR, FATAL_ERROR_WITH_CORE,
		INTERNAL_WARNING, INTERNAL_ERROR
	};

	ReporterMessage(Type arg_type, MsgThread* thread, const char* arg_msg)
		: OutputMessage<MsgThread>("ReporterMessage", thread)
		{ type = arg_type; msg = copy_string(arg_msg); }

	~ReporterMessage() 	 { delete [] msg; }

	virtual bool Process();

private:
	const char* msg;
	Type type;
};

#ifdef DEBUG
// A debug message from the child to be passed on to the DebugLogger.
class DebugMessage : public OutputMessage<MsgThread>
{
public:
	DebugMessage(DebugStream arg_stream, MsgThread* thread, const char* arg_msg)
		: OutputMessage<MsgThread>("DebugMessage", thread)
		{ stream = arg_stream; msg = copy_string(arg_msg); }

	virtual ~DebugMessage()	{ delete [] msg; }

	virtual bool Process()
		{
		debug_logger.Log(stream, "%s: %s", Object()->Name(), msg);
		return true;
		}
private:
	const char* msg;
	DebugStream stream;
};
#endif

}

////// Methods.

Message::~Message()
	{
	delete [] name;
	}

bool ReporterMessage::Process()
	{
	switch ( type ) {

	case INFO:
		reporter->Info("%s: %s", Object()->Name(), msg);
		break;

	case WARNING:
		reporter->Warning("%s: %s", Object()->Name(), msg);
		break;

	case ERROR:
		reporter->Error("%s: %s", Object()->Name(), msg);
		break;

	case FATAL_ERROR:
		reporter->FatalError("%s: %s", Object()->Name(), msg);
		break;

	case FATAL_ERROR_WITH_CORE:
		reporter->FatalErrorWithCore("%s: %s", Object()->Name(), msg);
		break;

	case INTERNAL_WARNING:
		reporter->InternalWarning("%s: %s", Object()->Name(), msg);
		break;

	case INTERNAL_ERROR :
		reporter->InternalError("%s: %s", Object()->Name(), msg);
		break;

	default:
		reporter->InternalError("unknown ReporterMessage type %d", type);
	}

	return true;
	}

MsgThread::MsgThread() : BasicThread(), queue_in(this, 0), queue_out(0, this)
	{
	cnt_sent_in = cnt_sent_out = 0;
	finished = false;
	thread_mgr->AddMsgThread(this);
	}

// Set by Bro's main signal handler.
extern int signal_val;

void MsgThread::OnPrepareStop()
	{
	if ( finished || Killed() )
		return;

	// Signal thread to terminate and wait until it has acknowledged.
	SendIn(new FinishMessage(this, network_time), true);
	}

void MsgThread::OnStop()
	{
	int signal_count = 0;
	int old_signal_val = signal_val;
	signal_val = 0;

	int cnt = 0;
	uint64_t last_size = 0;
	uint64_t cur_size = 0;

	// XX fprintf(stderr, "WAITING for thread %s to stop ...\n", Name());

	while ( ! (finished || Killed() ) )
		{
                // Terminate if we get another kill signal.
		if ( signal_val == SIGTERM || signal_val == SIGINT )
			{
			++signal_count;

			if ( signal_count == 1 )
				{
				// Abort all threads here so that we won't hang next
				// on another one.
				fprintf(stderr, "received signal while waiting for thread %s, aborting all ...\n", Name());
				thread_mgr->KillThreads();
				}
			else
				{
				// More than one signal. Abort processing
				// right away. on another one.
				fprintf(stderr, "received another signal while waiting for thread %s, aborting processing\n", Name());
				exit(1);
				}

			signal_val = 0;
			}

		queue_in.WakeUp();

		usleep(1000);
		}

	signal_val = old_signal_val;
	}

void MsgThread::OnKill()
	{
	// Send a message to unblock the reader if its currently waiting for
	// input. This is just an optimization to make it terminate more
	// quickly, even without the message it will eventually time out.
	queue_in.WakeUp();
	}

void MsgThread::Heartbeat()
	{
	SendIn(new HeartbeatMessage(this, network_time, current_time()));
	}

void MsgThread::HeartbeatInChild()
	{
	string n = Fmt("bro: %s (%" PRIu64 "/%" PRIu64 ")", Name(),
		cnt_sent_in - queue_in.Size(),
		cnt_sent_out - queue_out.Size());

	SetOSName(n.c_str());
	}

void MsgThread::Finished()
	{
	// This is thread-safe "enough", we're the only one ever writing
	// there.
	finished = true;
	}

void MsgThread::Info(const char* msg)
	{
	SendOut(new ReporterMessage(ReporterMessage::INFO, this, msg));
	}

void MsgThread::Warning(const char* msg)
	{
	SendOut(new ReporterMessage(ReporterMessage::WARNING, this, msg));
	}

void MsgThread::Error(const char* msg)
	{
	SendOut(new ReporterMessage(ReporterMessage::ERROR, this, msg));
	}

void MsgThread::FatalError(const char* msg)
	{
	SendOut(new ReporterMessage(ReporterMessage::FATAL_ERROR, this, msg));
	}

void MsgThread::FatalErrorWithCore(const char* msg)
	{
	SendOut(new ReporterMessage(ReporterMessage::FATAL_ERROR_WITH_CORE, this, msg));
	}

void MsgThread::InternalWarning(const char* msg)
	{
	SendOut(new ReporterMessage(ReporterMessage::INTERNAL_WARNING, this, msg));
	}

void MsgThread::InternalError(const char* msg)
	{
	// This one aborts immediately.
	fprintf(stderr, "internal error in thread: %s\n", msg);
	abort();
	}

#ifdef DEBUG

void MsgThread::Debug(DebugStream stream, const char* msg)
	{
	SendOut(new DebugMessage(stream, this, msg));
	}

#endif

void MsgThread::SendIn(BasicInputMessage* msg, bool force)
	{
	if ( Terminating() && ! force )
		{
		delete msg;
		return;
		}

	DBG_LOG(DBG_THREADING, "Sending '%s' to %s ...", msg->Name(), Name());

	queue_in.Put(msg);
	++cnt_sent_in;
	}


void MsgThread::SendOut(BasicOutputMessage* msg, bool force)
	{
	if ( Terminating() && ! force )
		{
		delete msg;
		return;
		}

	queue_out.Put(msg);

	++cnt_sent_out;
	}

BasicOutputMessage* MsgThread::RetrieveOut()
	{
	BasicOutputMessage* msg = queue_out.Get();
	if ( ! msg )
		return 0;

	DBG_LOG(DBG_THREADING, "Retrieved '%s' from %s",  msg->Name(), Name());

	return msg;
	}

BasicInputMessage* MsgThread::RetrieveIn()
	{
	BasicInputMessage* msg = queue_in.Get();

	if ( ! msg )
		return 0;

#ifdef DEBUG
	string s = Fmt("Retrieved '%s' in %s",  msg->Name(), Name());
	Debug(DBG_THREADING, s.c_str());
#endif

	return msg;
	}

void MsgThread::Run()
	{
	while ( ! (finished || Killed() ) )
		{
		BasicInputMessage* msg = RetrieveIn();

		if ( ! msg )
			continue;

		bool result = msg->Process();

		if ( ! result )
			{
			string s = Fmt("%s failed, terminating thread (MsgThread)", Name());
			Error(s.c_str());
			break;
			}

		delete msg;
		}

	// In case we haven't send the finish method yet, do it now. Reading
	// global network_time here should be fine, it isn't changing
	// anymore.
	if ( ! finished )
		{
		OnFinish(network_time);
		Finished();
		}
	}

void MsgThread::GetStats(Stats* stats)
	{
	stats->sent_in = cnt_sent_in;
	stats->sent_out = cnt_sent_out;
	stats->pending_in = queue_in.Size();
	stats->pending_out = queue_out.Size();
	queue_in.GetStats(&stats->queue_in_stats);
	queue_out.GetStats(&stats->queue_out_stats);
	}

