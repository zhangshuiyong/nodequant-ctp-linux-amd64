#pragma once
#include <uv.h>

#define MSG_MAX_COUNT 200
///////////////////////////////内部使用
#define T_CONNECT_RE 1
#define T_LOGIN_RE 2
#define T_LOGOUT_RE 3
#define T_CONFIRMSETTLEMENT_RE 4
#define T_QRYINSTRUMENT_RE 5
#define T_QRYTRADINGACCOUNT_RE 6
#define T_QRYINVESTORPOSITION_RE 7
#define T_INVESTORPOSITIONDETAIL_RE 8
#define T_INSERTORDER_RE 9
#define T_INPUTORDERACTION_RE 10
#define T_MARGINRATE_RE 11
#define T_DEPTHMARKETDATA_RE 12
#define T_SETTLEMENTINFO_RE 13
#define T_SUBSCRIBE_MARKET_DATA_RE 14
#define T_UNSUBSCRIBE_MARKET_DATA_RE 15
#define T_DISCONNECT_RE	16
#define T_Authenticate_RE 17
#define T_QueryCommissionRate_RE 18
#define T_QuerySettlementInfo_RE 19
///////////////////////////////外部使用
#define T_On_FrontConnected -1
#define T_On_FrontDisconnected -2
#define T_On_RspError -3
#define T_On_RspUserLogin -4
#define T_On_RspSubMarketData -5
#define T_On_RtnDepthMarketData -6
#define T_On_RspUnSubMarketData -7
#define T_On_RspUserLogout -8

#define T_On_RspAuthenticate -9
#define T_On_RspSettlementInfoConfirm -10
#define T_On_RspQryInstrument -11
#define T_On_RspQryInvestorPosition -13
#define T_On_RspQryTradingAccount -14
#define T_On_RspOrderInsert -15
#define T_On_ErrRtnOrderInsert -16
#define T_On_RtnOrder -17
#define T_On_RtnTrade -18
#define T_On_RspOrderAction -19
#define T_On_ErrRtnOrderAction -20
#define T_On_RspQryInstrumentCommissionRate -21
#define T_On_RspQrySettlementInfo -22

struct ConnectField {
	char front_addr[200];
	char flowPath[400];
	int public_topic_type;
	int private_topic_type;
};

struct LookupCtpApiBaton {
	uv_work_t work;
	void(*callback)(int, void*);
	void* client_obj;
	void* args;
	int fuctionType;
	int requestID;
	int nResult;
	int uuid;//回调标识
	int nCount;
};

struct CbRtnField {
	uv_work_t work;
	int eFlag;//事件标识
	int nRequestID;
	int nReason;
	void* rtnField;
	void* rspInfo;
	bool bIsLast;
};


struct OnEventCbRtnField {
	int eFlag;//事件标识
	int nRequestID;
	int nReason;
	void* rtnField;
	void* rspInfo;
	bool bIsLast;
};

struct CbWrap {
	void(*callback)(CbRtnField *data);
};
