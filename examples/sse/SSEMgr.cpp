#include "hlog.h"
#include "SSEMgr.h"

SSEMgr& SSEMgr::instance()
{
	static SSEMgr sInstance;
	return sInstance;
}


SSEMgr::SSEMgr()
{

}

void SSEMgr::add(LONGLONG id, HttpContextPtr ctx)
{
	hlogi("SSEMgr add:%I64d", id);
	ctx->userdata = (void*)id;
	ctx->writer->onclose = std::bind(&SSEMgr::on_ctx_close, &SSEMgr::instance(), id);
	if (ctx->writer->isConnected())
	{
		std::lock_guard<std::mutex> lk(m_mtx);
		m_id_ctxs[id] = ctx;
	}

	//开始刷新数据
	//GameStatusMgr::instance();
}

void SSEMgr::broadcast(const std::string& data)
{
	std::unordered_map<LONGLONG, HttpContextPtr> id_ctxs;
	{
		std::lock_guard<std::mutex> lk(m_mtx);
		id_ctxs = m_id_ctxs;
	}

	for (auto &it:id_ctxs)
	{
		if (it.second && it.second->writer->isConnected())
			it.second->writer->SSEvent(data);
		else if (it.second) //on_ctx_close 可能触发多次，但是不影响
			on_ctx_close((LONGLONG)it.second->userdata);
	}
}

bool SSEMgr::empty()
{
	std::lock_guard<std::mutex> lk(m_mtx);
	return m_id_ctxs.empty();
}

void SSEMgr::on_ctx_close(LONGLONG id)
{
	hlogi("SSEMgr erase:%I64d", id);
	std::lock_guard<std::mutex> lk(m_mtx);
	m_id_ctxs.erase(id);
}


