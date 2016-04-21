/*
 * master.cpp
 *
 *  Created on: 2013-08-22
 *      Author: yinqiwen
 */
#include "repl.hpp"
#include <fcntl.h>
#include <sys/stat.h>
#include "repl.hpp"

#define MAX_SEND_CACHE_SIZE 8192

OP_NAMESPACE_BEGIN
    enum SyncState
    {
        SYNC_STATE_INVALID, SYNC_STATE_START, SYNC_STATE_WAITING_SNAPSHOT, SYNC_STATE_SYNCING_SNAPSHOT,
        //SYNC_STATE_SYNCING_WAL,
        SYNC_STATE_SYNCED,
    };

    struct SlaveSyncContext
    {
            Snapshot* snapshot;
            Channel* conn;
            std::string repl_key;
            int64 sync_offset;
            int64 ack_offset;
            uint64 sync_cksm;
            uint32 acktime;
            uint32 port;
            int repldbfd;
            bool isRedisSlave;
            uint8 state;
            SlaveSyncContext() :
                    snapshot(NULL), conn(NULL), sync_offset(0), ack_offset(0), sync_cksm(0), acktime(0), port(0), repldbfd(-1), isRedisSlave(false), state(
                            SYNC_STATE_INVALID)
            {
            }
            std::string GetAddress()
            {
                std::string address;
                Address* remote = const_cast<Address*>(conn->GetRemoteAddress());
                if (InstanceOf<SocketHostAddress>(remote).OK)
                {
                    SocketHostAddress* un = (SocketHostAddress*) remote;
                    address = "ip=";
                    address += un->GetHost();
                    address += ",port=";
                    address += stringfromll(port);
                }
                return address;
            }
            ~SlaveSyncContext()
            {
            }
    };

    Master::Master() :
            m_repl_noslaves_since(0)
    {
    }

    int Master::Init()
    {
//        struct HeartbeatTask: public Runnable
//        {
//                Master* serv;
//                HeartbeatTask(Master* s) :
//                        serv(s)
//                {
//                }
//                void Run()
//                {
//                    serv->OnHeartbeat();
//                }
//        };
//        struct RoutineTask: public Runnable
//        {
//                Master* serv;
//                RoutineTask(Master* s) :
//                        serv(s)
//                {
//                }
//                void Run()
//                {
//                    serv->ClearNilSlave();
//                }
//        };
//
//        g_repl->GetTimer().ScheduleHeapTask(new HeartbeatTask(this), g_db->GetConfig().repl_ping_slave_period, g_db->GetConfig().repl_ping_slave_period,
//                SECONDS);
//        g_repl->GetTimer().ScheduleHeapTask(new RoutineTask(this), 1, 1, SECONDS);
        return 0;
    }

//    void Master::ClearNilSlave()
//    {
//        SlaveContextSet::iterator it = m_slaves.begin();
//        while (it != m_slaves.end())
//        {
//            SlaveContext* ctx = *it;
//            if (it->second == NULL)
//            {
//                it = m_slaves.erase(it);
//                continue;
//            }
//            it++;
//        }
//    }

    static void slave_pipeline_init(ChannelPipeline* pipeline, void* data)
    {
        pipeline->AddLast("decoder", new RedisCommandDecoder);
        pipeline->AddLast("encoder", new RedisCommandEncoder);
        pipeline->AddLast("handler", &(g_repl->GetMaster()));
    }

    static void slave_pipeline_finallize(ChannelPipeline* pipeline, void* data)
    {
        ChannelHandler* handler = pipeline->Get("decoder");
        DELETE(handler);
        handler = pipeline->Get("encoder");
        DELETE(handler);
    }

    static void snapshot_dump_success(Channel*, void* data)
    {
        Snapshot* snapshot = (Snapshot*) data;
        g_repl->GetMaster().FullResyncSlaves(snapshot);
    }
    static void snapshot_dump_fail(Channel*, void* data)
    {
        Snapshot* snapshot = (Snapshot*) data;
        g_repl->GetMaster().CloseSlaveBySnapshot(snapshot);
    }

    static int snapshot_dump_routine(RoutineState state, Snapshot* snapshot, void* cb)
    {
        Master* m = (Master*) cb;
        SlaveSyncContextSet::iterator fit = m->m_slaves.begin();
        if (state == ROUTINE_SUCCESS)
        {
            g_repl->GetIOService().AsyncIO(0, snapshot_dump_success, snapshot);
        }
        else if (state == ROUTINE_FAIL)
        {
            g_repl->GetIOService().AsyncIO(0, snapshot_dump_fail, snapshot);
        }
        else if (state == ROUTINING)
        {
            g_repl->GetIOService().Continue();
        }
        return 0;
    }

    int Master::PingSlaves()
    {
        //Just process instructions  since the soft signal may loss
        if (!g_db->GetConf().master_host.empty())
        {
            //let master feed 'ping' command
            return 0;
        }
        if (m_slaves.empty())
        {
            if (0 == m_repl_noslaves_since)
            {
                m_repl_noslaves_since = time(NULL);
            }
            return 0;
        }
        m_repl_noslaves_since = 0;
        SlaveSyncContextSet::iterator fit = m_slaves.begin();
        bool synced_slaves_count = 0;
        while (fit != m_slaves.end())
        {
            SlaveSyncContext* slave = *fit;
            if (slave->state == SYNC_STATE_WAITING_SNAPSHOT)
            {
                Buffer newline;
                newline.Write("\n", 1);
                slave->conn->Write(newline);
                //waiting_dump_slave_count++;
            }
            if (slave->state == SYNC_STATE_SYNCED)
            {
                synced_slaves_count++;
            }
            fit++;
        }

        //only master instance can generate ping command to slaves
        if (synced_slaves_count > 0 && g_db->GetConf().master_host.empty())
        {
            m_repl_noslaves_since = 0;
            Buffer ping;
            ping.Printf("ping\r\n");
            g_repl->GetReplLog().WriteWAL(ping);
            SyncWAL();
            DEBUG_LOG("Ping slaves.");
        }
        return 0;
    }

    void Master::FullResyncSlaves(Snapshot* snapshot)
    {
        SlaveSyncContextSet::iterator it = m_slaves.begin();
        while (it != m_slaves.end())
        {
            SlaveSyncContext* slave = *it;
            if (slave != NULL && slave->snapshot == snapshot)
            {
                SendSnapshotToSlave(slave);
            }
            it++;
        }
    }
    void Master::CloseSlaveBySnapshot(Snapshot* snapshot)
    {
        SlaveSyncContextSet::iterator it = m_slaves.begin();
        while (it != m_slaves.end())
        {
            SlaveSyncContext* slave = *it;
            if (slave != NULL && slave->snapshot == snapshot)
            {
                //SendSnapshotToSlave(slave);
            }
            it++;
        }
    }

    static void OnSnapshotFileSendComplete(void* data)
    {
        SlaveSyncContext* slave = (SlaveSyncContext*) data;
        close(slave->repldbfd);
        slave->repldbfd = -1;
        slave->state = SYNC_STATE_SYNCED;
        slave->snapshot = NULL;
    }

    static void OnSnapshotFileSendFailure(void* data)
    {
        SlaveSyncContext* slave = (SlaveSyncContext*) data;
        close(slave->repldbfd);
        slave->repldbfd = -1;
    }

    void Master::SendSnapshotToSlave(SlaveSyncContext* slave)
    {
        slave->state = SYNC_STATE_SYNCING_SNAPSHOT;
        //FULLRESYNC
        Buffer msg;
        slave->sync_offset = slave->snapshot->CachedReplOffset();
        slave->sync_cksm = slave->snapshot->CachedReplCksm();
        if (slave->isRedisSlave)
        {
            msg.Printf("+FULLRESYNC %s %lld\r\n", g_repl->GetReplLog().GetReplKey(), slave->sync_offset);
        }
        else
        {
            msg.Printf("+FULLRESYNC %s %lld %llu\r\n", g_repl->GetReplLog().GetReplKey(), slave->sync_offset, slave->sync_cksm);
        }
        slave->conn->Write(msg);
        std::string dump_file_path = slave->snapshot->GetPath();
        SendFileSetting setting;
        setting.fd = open(dump_file_path.c_str(), O_RDONLY);
        if (-1 == setting.fd)
        {
            int err = errno;
            ERROR_LOG("Failed to open file:%s for reason:%s", dump_file_path.c_str(), strerror(err));
            slave->conn->Close();
            return;
        }
        setting.file_offset = 0;
        struct stat st;
        fstat(setting.fd, &st);
        Buffer header;
        header.Printf("$%llu\r\n", st.st_size);
        slave->conn->Write(header);

        setting.file_rest_len = st.st_size;
        setting.on_complete = OnSnapshotFileSendComplete;
        setting.on_failure = OnSnapshotFileSendFailure;
        setting.data = slave;
        slave->repldbfd = setting.fd;
        slave->conn->SendFile(setting);
    }

    static int send_wal_toslave(const void* log, size_t loglen, void* data)
    {
        SlaveSyncContext* slave = (SlaveSyncContext*) data;
        Buffer msg((char*) log, 0, loglen);
        slave->conn->Write(msg);
        slave->sync_offset += loglen;
        //INFO_LOG("####%d %d %d", loglen, slave->sync_offset, g_repl->WALEndOffset());
        if (slave->sync_offset == g_repl->GetReplLog().WALEndOffset())
        {
            slave->conn->GetWritableOptions().auto_disable_writing = true;
        }
        else
        {
            slave->conn->GetWritableOptions().auto_disable_writing = false;
        }
        slave->conn->EnableWriting();
        return 0;
    }

    void Master::SyncWAL(SlaveSyncContext* slave)
    {
        if (slave->sync_offset < g_repl->GetReplLog().WALStartOffset() || slave->sync_offset > g_repl->GetReplLog().WALEndOffset())
        {
            slave->conn->Close();
            ERROR_LOG("Slave synced offset:%llu is invalid in offset range[%llu-%llu] for wal.", slave->sync_offset, g_repl->GetReplLog().WALStartOffset(),
                    g_repl->GetReplLog().WALEndOffset());
            return;
        }
        if (slave->sync_offset < g_repl->GetReplLog().WALEndOffset())
        {
            swal_replay(g_repl->GetReplLog().GetWAL(), slave->sync_offset, MAX_SEND_CACHE_SIZE, send_wal_toslave, slave);
        }
    }

    void Master::SyncWAL()
    {
        SlaveSyncContextSet::iterator it = m_slaves.begin();
        bool no_lag_slave = true;
        while (it != m_slaves.end())
        {
            SlaveSyncContext* slave = *it;
            if (slave != NULL)
            {
                if (slave->state == SYNC_STATE_SYNCED)
                {
                    if (slave->sync_offset != g_repl->GetReplLog().WALStartOffset())
                    {
                        SyncWAL(slave);
                        no_lag_slave = false;
                    }
                }
                else
                {
                    no_lag_slave = false;
                }

            }
            it++;
        }
//        if (no_lag_slave)
//        {
//            swal_clear_replay_cache(g_repl->GetReplLog().GetWAL());
//        }
    }

    void Master::SyncSlave(SlaveSyncContext* slave)
    {
        if (!g_db->GetConf().master_host.empty())
        {
            if (!g_repl->GetSlave().IsSynced())
            {
                //just close slave connection if current instance not synced with remote master
                slave->conn->Close();
                return;
            }
        }

        if (slave->repl_key.empty())
        {
            //redis 2.6/2.4
            slave->state = SYNC_STATE_SYNCING_SNAPSHOT;
        }
        else
        {
            Buffer msg;
            bool fullsync = true;
            if (slave->repl_key == g_repl->GetReplLog().GetReplKey())
            {
                if (!g_repl->GetReplLog().IsValidOffsetCksm(slave->sync_offset, slave->sync_cksm))
                {
                    fullsync = true;
                }
                else
                {
                    fullsync = false;
                }
            }
            if (!fullsync)
            {
                msg.Printf("+CONTINUE\r\n");
                slave->conn->Write(msg);
                slave->state = SYNC_STATE_SYNCED;
                SyncWAL();
            }
            else
            {
                WARN_LOG("Create snapshot for full resync for slave runid:%s offset:%llu cksm:%llu, while current WAL runid:%s offset:%llu cksm:%llu",
                        slave->repl_key.c_str(), slave->sync_offset, slave->sync_cksm, g_repl->GetReplLog().GetReplKey(), g_repl->GetReplLog().WALEndOffset(),
                        g_repl->GetReplLog().WALCksm());
                slave->state = SYNC_STATE_WAITING_SNAPSHOT;
                slave->snapshot = g_snapshot_manager->GetSyncSnapshot(slave->isRedisSlave ? REDIS_DUMP : ARDB_DUMP, snapshot_dump_routine, this);
            }
        }
    }

    void Master::ChannelClosed(ChannelHandlerContext& ctx, ChannelStateEvent& e)
    {
        SlaveSyncContext* slave = (SlaveSyncContext*) (ctx.GetChannel()->Attachment());
        if (NULL != slave)
        {
            WARN_LOG("Slave %s closed.", slave->GetAddress().c_str());
            m_slaves.erase(slave);
        }
    }
    void Master::ChannelWritable(ChannelHandlerContext& ctx, ChannelStateEvent& e)
    {
        uint32 conn_id = ctx.GetChannel()->GetID();
        SlaveSyncContext* slave = (SlaveSyncContext*) (ctx.GetChannel()->Attachment());
        if (NULL != slave)
        {
            if (slave->state == SYNC_STATE_SYNCED)
            {
                DEBUG_LOG("[Master]Slave sync from %lld to %llu at state:%u", slave->sync_offset, g_repl->GetReplLog().WALEndOffset(), slave->state);
                SyncWAL(slave);
            }
        }
        else
        {
            WARN_LOG("[Master]No slave found for:%u", conn_id);
        }
    }
    void Master::MessageReceived(ChannelHandlerContext& ctx, MessageEvent<RedisCommandFrame>& e)
    {
        RedisCommandFrame* cmd = e.GetMessage();
        DEBUG_LOG("Master recv cmd from slave:%s", cmd->ToString().c_str());
        SlaveSyncContext* slave = (SlaveSyncContext*) (ctx.GetChannel()->Attachment());
        if (!strcasecmp(cmd->GetCommand().c_str(), "replconf"))
        {
            if (cmd->GetArguments().size() == 2 && !strcasecmp(cmd->GetArguments()[0].c_str(), "ack"))
            {
                int64 offset;
                if (string_toint64(cmd->GetArguments()[1], offset))
                {
                    if (NULL != slave)
                    {
                        slave->acktime = time(NULL);
                        slave->ack_offset = offset;
                    }
                }
            }
        }
    }

    static void OnAddSlave(Channel* ch, void* data)
    {
        SlaveSyncContext* conn = (SlaveSyncContext*) data;
        g_repl->GetMaster().AddSlave(conn);
    }

    static void destroy_slave_conn(void* s)
    {
        delete (SlaveSyncContext*) s;
    }
    static SlaveSyncContext& getSlaveContext(Channel* slave)
    {
        if (slave->Attachment() == NULL)
        {
            SlaveSyncContext* s = new SlaveSyncContext;
            s->conn = slave;
            slave->Attach(s, destroy_slave_conn);
        }
        SlaveSyncContext* slave_conn = (SlaveSyncContext*) slave->Attachment();
        return *slave_conn;
    }

    void Master::AddSlave(SlaveSyncContext* slave)
    {
        DEBUG_LOG("Add slave %s", slave->GetAddress().c_str());
        g_repl->GetIOService().AttachChannel(slave->conn, true);
        slave->state = SYNC_STATE_START;
        slave->conn->ClearPipeline();
        if (g_db->GetConf().repl_disable_tcp_nodelay)
        {
            ChannelOptions newoptions = slave->conn->GetOptions();
            newoptions.tcp_nodelay = false;
            slave->conn->Configure(newoptions);
        }
        slave->conn->SetChannelPipelineInitializor(slave_pipeline_init, NULL);
        slave->conn->SetChannelPipelineFinalizer(slave_pipeline_finallize, NULL);
        slave->conn->GetWritableOptions().auto_disable_writing = false;

        m_slaves.insert(slave);
        SyncSlave(slave);
    }

    void Master::AddSlave(Channel* slave, RedisCommandFrame& cmd)
    {
        INFO_LOG("[Master]Recv sync command:%s", cmd.ToString().c_str());
        slave->Flush();
        SlaveSyncContext& ctx = getSlaveContext(slave);
        if (cmd.GetType() == REDIS_CMD_SYNC)
        {
            //Redis 2.6/2.4 send 'sync'
            ctx.isRedisSlave = true;
            ctx.sync_offset = -1;
        }
        else
        {
            ctx.repl_key = cmd.GetArguments()[0];
            const std::string& offset_str = cmd.GetArguments()[1];
            if (!string_toint64(offset_str, ctx.sync_offset))
            {
                ERROR_LOG("Invalid offset argument:%s", offset_str.c_str());
                slave->Close();
                return;
            }
            ctx.isRedisSlave = true;
            for (uint32 i = 2; i < cmd.GetArguments().size(); i += 2)
            {
                if (cmd.GetArguments()[i] == "cksm")
                {
                    ctx.isRedisSlave = false;
                    if (!string_touint64(cmd.GetArguments()[i + 1], ctx.sync_cksm))
                    {
                        ERROR_LOG("Invalid checksum argument:%s", cmd.GetArguments()[i + 1].c_str());
                        slave->Close();
                        return;
                    }
                }
            }
        }
        slave->GetService().DetachChannel(slave, true);
        if (g_repl->GetIOService().IsInLoopThread())
        {
            AddSlave (&conn);
        }
        else
        {
            g_repl->GetIOService().AsyncIO(0, OnAddSlave, &ctx);
        }
    }

    void Master::DisconnectAllSlaves()
    {
        SlaveSyncContextSet::iterator it = m_slaves.begin();
        while (it != m_slaves.end())
        {
            SlaveSyncContext* slave = *it;
            if (NULL != slave)
            {
                slave->conn->Close();
            }
            it++;
        }
    }

    size_t Master::ConnectedSlaves()
    {
        return m_slaves.size();
    }

    void Master::PrintSlaves(std::string& str)
    {
        uint32 i = 0;
        char buffer[1024];
        SlaveSyncContextSet::iterator it = m_slaves.begin();
        while (it != m_slaves.end())
        {
            const char* state = "ok";
            SlaveSyncContext* slave = *it;
            if (NULL == slave)
            {
                it++;
                continue;
            }
            switch (slave->state)
            {
                case SYNC_STATE_WAITING_SNAPSHOT:
                {
                    state = "wait_bgsave";
                    break;
                }
                case SYNC_STATE_SYNCING_SNAPSHOT:
                {
                    state = "send_bulk";
                    break;
                }
                case SYNC_STATE_SYNCED:
                {
                    state = "online";
                    break;
                }
                default:
                {
                    state = "invalid state";
                    break;
                }
            }

            uint32 lag = time(NULL) - slave->acktime;
            sprintf(buffer, "slave%u:%s,state=%s,"
                    "offset=%" PRId64",lag=%u, o_buffer_size:%u, o_buffer_capacity:%u\r\n", i, slave->GetAddress().c_str(), state, slave->sync_offset, lag,
                    slave->conn->WritableBytes(), slave->conn->GetOutputBuffer().Capacity());
            it++;
            i++;
            str.append(buffer);
        }
    }

    void Master::SetSlavePort(Channel* slave, uint32 port)
    {
        getSlaveContext(slave).port = port;
    }

    Master::~Master()
    {
    }
OP_NAMESPACE_END
