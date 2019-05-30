#pragma once
#include <queue>
#include <map>
#include <uv.h>
#include <node_api.h>

#include "ThostFtdcTraderApi.h"

#include "nodequant_struct.h"

using namespace std;

class CTPTraderClient :public CThostFtdcTraderSpi
{
private:

	CTPTraderClient();
	~CTPTraderClient();

	//类的Prototype对象
	static napi_ref prototype;
	//类的Js执行环境
	napi_env env_;
	//c++类的Wrap(隐藏)句柄
	napi_ref wrapper_;


	//用于限定特定event类型，只处理这些event类型
	static map<string, int> eventName_map;
	static void initEventNameMap();
	//每个实例都有特定event类型对应的1个Js层callback函数,exit的时候要清空这个和释放这个Js层callback函数
	static map<int, napi_ref> callback_map;

	//libuv 通道
	uv_async_t channel;
	//eventQueue互斥锁
	uv_mutex_t eventQueueMutex;
	//vector非线程安全
	//多线程读写事件队列
	vector<OnEventCbRtnField*> eventQueue;
	static void MainThreadCallback(uv_async_t* pChannel);
	static void ChannelClosedCallback(uv_async_t* pChannel);

	//CTP的主线程Api对象
	CThostFtdcTraderApi* Api;

	//Api接口请求号
	static int requestID;

	//Js层主动调用CTP的Api函数
	int invoke(void* field, int fuctionType, int requestID);

	//Spi线程调用Js层事件响应callback
	void on_invoke(int event_type, void* _stru, CThostFtdcRspInfoField *pRspInfo_org, int nRequestID, bool bIsLast);
	
	//Spi子线程写eventQueue
	void queueEvent(OnEventCbRtnField* event);

	//Api主线程读eventQueue
	void processEventQueue();

	//定义为类私有方法，需要使用类私有属性执行环境env_,转换Js层变量
	void process_event(OnEventCbRtnField* cbTrnField);
    void pkg_cb_rsperror(OnEventCbRtnField* data, napi_value* cbArgs);
	void pkg_rspinfo(CThostFtdcRspInfoField* pRspInfo, napi_value* cbArgs);
	void pkg_cb_rspauthenticate(OnEventCbRtnField* data, napi_value* cbArgs);
	void pkg_cb_userlogin(OnEventCbRtnField* data, napi_value* cbArray);
	void pkg_cb_settlementInfo(OnEventCbRtnField* data, napi_value* cbArgs);
	void pkg_cb_confirmsettlement(OnEventCbRtnField* data, napi_value* cbArgs);
	void pkg_cb_qryinstrument(OnEventCbRtnField* data, napi_value* cbArgs);
	void pkg_cb_qrytradingaccount(OnEventCbRtnField* data, napi_value* cbArgs);
	void pkg_cb_qryinvestorposition(OnEventCbRtnField* data, napi_value* cbArgs);
	void pkg_cb_rspinsertorder(OnEventCbRtnField* data, napi_value* cbArgs);
	void pkg_cb_errrtnorderinsert(OnEventCbRtnField* data, napi_value* cbArgs);
	void pkg_cb_rtnorder(OnEventCbRtnField* data, napi_value* cbArgs);
	void pkg_cb_rtntrade(OnEventCbRtnField* data, napi_value* cbArgs);
	void pkg_cb_rsporderaction(OnEventCbRtnField* data, napi_value* cbArgs);
	void pkg_cb_errrtnorderaction(OnEventCbRtnField* data, napi_value* cbArgs);
	void pkg_cb_rspuserlogout(OnEventCbRtnField* data, napi_value* cbArgs);
	void pkg_cb_RspQryInstrumentCommissionRate(OnEventCbRtnField* data, napi_value* cbArgs);


	///1.当客户端与交易后台建立起通信连接时（还未登录前），该方法被调用。
	virtual void OnFrontConnected();
	///2.当客户端与交易后台通信连接断开时，该方法被调用。当发生这个情况后，API会自动重新连接，客户端可不做处理。
	///@param nReason 错误原因
	///        0x1001 4097 网络读失败 
	///        0x1002 4098 网络写失败
	///        0x2001 8193 接收心跳超时
	///        0x2002 8194 发送心跳失败
	///        0x2003 8195 收到错误报文
	virtual void OnFrontDisconnected(int nReason);

	///3.错误应答
	virtual void OnRspError(CThostFtdcRspInfoField *pRspInfo, int nRequestID, bool bIsLast);

	///4.客户端认证响应
	virtual void OnRspAuthenticate(CThostFtdcRspAuthenticateField *pRspAuthenticateField, CThostFtdcRspInfoField *pRspInfo, int nRequestID, bool bIsLast);

	///5.登录请求响应
	virtual void OnRspUserLogin(CThostFtdcRspUserLoginField *pRspUserLogin, CThostFtdcRspInfoField *pRspInfo, int nRequestID, bool bIsLast);
	
	///请求查询投资者结算结果响应
	virtual void OnRspQrySettlementInfo(CThostFtdcSettlementInfoField *pSettlementInfo, CThostFtdcRspInfoField *pRspInfo, int nRequestID, bool bIsLast);

	///6.投资者结算结果确认响应
	virtual void OnRspSettlementInfoConfirm(CThostFtdcSettlementInfoConfirmField *pSettlementInfoConfirm, CThostFtdcRspInfoField *pRspInfo, int nRequestID, bool bIsLast);

	///7.请求查询合约响应
	virtual void OnRspQryInstrument(CThostFtdcInstrumentField *pInstrument, CThostFtdcRspInfoField *pRspInfo, int nRequestID, bool bIsLast);

	///8.登出请求响应
	virtual void OnRspUserLogout(CThostFtdcUserLogoutField *pUserLogout, CThostFtdcRspInfoField *pRspInfo, int nRequestID, bool bIsLast);
	///9.请求查询投资者持仓响应
	virtual void OnRspQryInvestorPosition(CThostFtdcInvestorPositionField *pInvestorPosition, CThostFtdcRspInfoField *pRspInfo, int nRequestID, bool bIsLast);
	///10.请求查询资金账户响应
	virtual void OnRspQryTradingAccount(CThostFtdcTradingAccountField *pTradingAccount, CThostFtdcRspInfoField *pRspInfo, int nRequestID, bool bIsLast);
	///11.报单录入请求响应
	//交易核心对收到的交易序列报文做合法性检查，检查出错误的交易申请报文后就会返回给交易前置一个
	//包含错误信息的报单响应报文，交易前置立即将该报文信息转发给交易终端。
	virtual void OnRspOrderInsert(CThostFtdcInputOrderField *pInputOrder, CThostFtdcRspInfoField *pRspInfo, int nRequestID, bool bIsLast);
	///12.报单录入错误回报
	//此接口仅在报单被 CTP 端拒绝时被调用用来进行报错。
	virtual void OnErrRtnOrderInsert(CThostFtdcInputOrderField *pInputOrder, CThostFtdcRspInfoField *pRspInfo);
	///13.报单通知
	//交易核心向交易所申请该报单插入的申请报文，会被调用多次
	//1.交易所撤销 2.接受该报单时 3.该报单成交时 4.交易所端校验失败OrderStatusMsg
	//撤单通知
	//如果交易所认为撤单指令合法，同样会返回对应报单的新状态（OnRtnOrder）。
	//交易核心确认了撤单指令的合法性后，将该撤单指令提交给交易所，同时返回对应报单的新状态。（OnRtnOrder）
	virtual void OnRtnOrder(CThostFtdcOrderField *pOrder);
	///14.成交通知
	//交易所中报单成交之后，一个报单回报（OnRtnOrder）和一个成交回报（OnRtnTrade）会被发送到客户端，报单回报
	//中报单的状态为“已成交”。但是仍然建议客户端将成交回报作为报单成交的标志，因为 CTP 的交易核心在
	//收到 OnRtnTrade 之后才会更新该报单的状态。
	virtual void OnRtnTrade(CThostFtdcTradeField *pTrade);

	///15.报单操作请求响应
	//撤单响应。交易核心返回的含有错误信息的撤单响应
	virtual void OnRspOrderAction(CThostFtdcInputOrderActionField *pInputOrderAction, CThostFtdcRspInfoField *pRspInfo, int nRequestID, bool bIsLast);

	///16.报单操作错误回报
	//交易所会再次验证撤单指令的合法性，如果交易所认为该指令不合法，交易核心通过此函数转发交易所给出的错误。
	virtual void OnErrRtnOrderAction(CThostFtdcOrderActionField *pOrderAction, CThostFtdcRspInfoField *pRspInfo);

	//17.手续费率查询请求
	virtual void OnRspQryInstrumentCommissionRate(CThostFtdcInstrumentCommissionRateField *pInstrumentCommissionRate, CThostFtdcRspInfoField *pRspInfo, int nRequestID, bool bIsLast);
public:

	//单例模式：1个进程一个TdClient
	static napi_ref Singleton;

	static napi_status Init(napi_env env);
	static napi_status NewInstance(napi_env env, napi_value arg, napi_value* instance);
	static void Destructor(napi_env env, void* nativeObject, void* finalize_hint);
	static napi_value New(napi_env env, napi_callback_info info);


	//td 暴露接口api
	static napi_value on(napi_env env, napi_callback_info info);
	static napi_value connect(napi_env env, napi_callback_info info);
	static napi_value getApiVersion(napi_env env, napi_callback_info info);
	static napi_value authenticate(napi_env env, napi_callback_info info);
	static napi_value login(napi_env env, napi_callback_info info);
	static napi_value querySettlementInfo(napi_env env, napi_callback_info info);
	static napi_value confirmSettlementInfo(napi_env env, napi_callback_info info);
	static napi_value queryInstrument(napi_env env, napi_callback_info info);
	static napi_value queryTradingAccount(napi_env env, napi_callback_info info);

	//查询持仓（汇总）
	//CTP 系统将持仓明细记录按合约，持仓方向，开仓日期（仅针对上期所，区分昨仓、今仓）进行汇总
	//持仓汇总记录中：
	//YdPosition 表示昨日收盘时持仓数量（≠ 当前的昨仓数量，静态，日间不随着开平仓而变化）
	//Position 表示当前持仓数量
	//TodayPosition 表示今新开仓
	//当前的昨仓数量 = ∑Position - ∑TodayPosition
	static napi_value queryInvestorPosition(napi_env env, napi_callback_info info);

	static napi_value sendOrder(napi_env env, napi_callback_info info);

	static napi_value cancelOrder(napi_env env, napi_callback_info info);

	static napi_value logout(napi_env env, napi_callback_info info);

	//查询手续费
	static napi_value queryCommissionRate(napi_env env, napi_callback_info info);

	static string CHString_To_UTF8(char* str);
};
