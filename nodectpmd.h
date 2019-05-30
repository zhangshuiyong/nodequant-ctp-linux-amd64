#pragma once
#include <string>
#include <map>
#include <queue>
#include <uv.h>
#include <node_api.h>
#include "ThostFtdcMdApi.h"

#include "nodequant_struct.h"

using namespace std;

class CTPMarketDataClient :public CThostFtdcMdSpi
{
private:

	CTPMarketDataClient();
	~CTPMarketDataClient();

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
	CThostFtdcMdApi* Api;

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
	void pkg_cb_userlogin(OnEventCbRtnField* data, napi_value* cbArgs);
	void pkg_cb_rspsubmarketdata(OnEventCbRtnField* data, napi_value* cbArgs);
	void pkg_cb_rtndepthmarketdata(OnEventCbRtnField* data, napi_value* cbArgs);
	void pkg_cb_unrspsubmarketdata(OnEventCbRtnField* data, napi_value* cbArgs);
	void pkg_cb_userlogout(OnEventCbRtnField* data, napi_value* cbArgs);



	///当客户端与交易后台建立起通信连接时（还未登录前），该方法被调用。
	virtual void OnFrontConnected();

	///当客户端与交易后台通信连接断开时，该方法被调用。当发生这个情况后，API会自动重新连接，客户端可不做处理。
	///@param nReason 错误原因
	///        0x1001 网络读失败
	///        0x1002 网络写失败
	///        0x2001 接收心跳超时
	///        0x2002 发送心跳失败
	///        0x2003 收到错误报文
	virtual void OnFrontDisconnected(int nReason);

	///错误应答
	virtual void OnRspError(CThostFtdcRspInfoField *pRspInfo, int nRequestID, bool bIsLast);

	///登录请求响应
	virtual void OnRspUserLogin(CThostFtdcRspUserLoginField *pRspUserLogin, CThostFtdcRspInfoField *pRspInfo, int nRequestID, bool bIsLast);
	///登出请求响应
	virtual void OnRspUserLogout(CThostFtdcUserLogoutField *pUserLogout, CThostFtdcRspInfoField *pRspInfo, int nRequestID, bool bIsLast);
	///订阅行情应答
	virtual void OnRspSubMarketData(CThostFtdcSpecificInstrumentField *pSpecificInstrument, CThostFtdcRspInfoField *pRspInfo, int nRequestID, bool bIsLast);
	///深度行情通知
	virtual void OnRtnDepthMarketData(CThostFtdcDepthMarketDataField *pDepthMarketData);

	///取消订阅行情应答
	virtual void OnRspUnSubMarketData(CThostFtdcSpecificInstrumentField *pSpecificInstrument, CThostFtdcRspInfoField *pRspInfo, int nRequestID, bool bIsLast);


public:
	//单例模式：1个进程一个MdClient
	static napi_ref Singleton;

	static napi_status Init(napi_env env);
	static napi_status NewInstance(napi_env env,napi_value arg,napi_value* instance);
	static void Destructor(napi_env env, void* nativeObject, void* finalize_hint);
	static napi_value New(napi_env env, napi_callback_info info);

	//md 暴露接口api
	static napi_value on(napi_env env, napi_callback_info info);
	static napi_value connect(napi_env env, napi_callback_info info);
	static napi_value login(napi_env env, napi_callback_info info);
	static napi_value subscribeMarketData(napi_env env, napi_callback_info info);
	static napi_value unSubscribeMarketData(napi_env env, napi_callback_info info);
	static napi_value logout(napi_env env, napi_callback_info info);
	static napi_value getApiVersion(napi_env env, napi_callback_info info);
	static napi_value getTradingDay(napi_env env, napi_callback_info info);
	static string CHString_To_UTF8(char* str);
};