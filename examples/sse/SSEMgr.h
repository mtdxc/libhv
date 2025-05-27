#pragma once
#include <unordered_map>
#include "hv/HttpContext.h"
#include <mutex>
class SSEMgr
{
public:
	static SSEMgr& instance();

	void add(LONGLONG id, HttpContextPtr ctx);
	void broadcast(const std::string& data);
	bool empty();
private:
	SSEMgr();
	void on_ctx_close(LONGLONG id);
private:
	std::mutex m_mtx;
	std::unordered_map<LONGLONG, HttpContextPtr> m_id_ctxs;
};

