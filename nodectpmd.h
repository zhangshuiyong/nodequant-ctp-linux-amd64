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

	//���Prototype����
	static napi_ref prototype;
	//���Jsִ�л���
	napi_env env_;
	//c++���Wrap(����)���
	napi_ref wrapper_;

	//�����޶��ض�event���ͣ�ֻ������Щevent����
	static map<string, int> eventName_map;
	static void initEventNameMap();
	//ÿ��ʵ�������ض�event���Ͷ�Ӧ��1��Js��callback����,exit��ʱ��Ҫ���������ͷ����Js��callback����
	static map<int, napi_ref> callback_map;


	//libuv ͨ��
	uv_async_t channel;
	//eventQueue������
	uv_mutex_t eventQueueMutex;
	//vector���̰߳�ȫ
	//���̶߳�д�¼�����
	vector<OnEventCbRtnField*> eventQueue;
	static void MainThreadCallback(uv_async_t* pChannel);
	static void ChannelClosedCallback(uv_async_t* pChannel);

	//CTP�����߳�Api����
	CThostFtdcMdApi* Api;

	//Api�ӿ������
	static int requestID;

	//Js����������CTP��Api����
	int invoke(void* field, int fuctionType, int requestID);

	//Spi�̵߳���Js���¼���Ӧcallback
	void on_invoke(int event_type, void* _stru, CThostFtdcRspInfoField *pRspInfo_org, int nRequestID, bool bIsLast);
	
	//Spi���߳�дeventQueue
	void queueEvent(OnEventCbRtnField* event);
	//Api���̶߳�eventQueue
	void processEventQueue();

	//����Ϊ��˽�з�������Ҫʹ����˽������ִ�л���env_,ת��Js�����
	void process_event(OnEventCbRtnField* cbTrnField);
	void pkg_cb_rsperror(OnEventCbRtnField* data, napi_value* cbArgs);
    void pkg_rspinfo(CThostFtdcRspInfoField* pRspInfo, napi_value* cbArgs);
	void pkg_cb_userlogin(OnEventCbRtnField* data, napi_value* cbArgs);
	void pkg_cb_rspsubmarketdata(OnEventCbRtnField* data, napi_value* cbArgs);
	void pkg_cb_rtndepthmarketdata(OnEventCbRtnField* data, napi_value* cbArgs);
	void pkg_cb_unrspsubmarketdata(OnEventCbRtnField* data, napi_value* cbArgs);
	void pkg_cb_userlogout(OnEventCbRtnField* data, napi_value* cbArgs);



	///���ͻ����뽻�׺�̨������ͨ������ʱ����δ��¼ǰ�����÷��������á�
	virtual void OnFrontConnected();

	///���ͻ����뽻�׺�̨ͨ�����ӶϿ�ʱ���÷��������á���������������API���Զ��������ӣ��ͻ��˿ɲ�������
	///@param nReason ����ԭ��
	///        0x1001 �����ʧ��
	///        0x1002 ����дʧ��
	///        0x2001 ����������ʱ
	///        0x2002 ��������ʧ��
	///        0x2003 �յ�������
	virtual void OnFrontDisconnected(int nReason);

	///����Ӧ��
	virtual void OnRspError(CThostFtdcRspInfoField *pRspInfo, int nRequestID, bool bIsLast);

	///��¼������Ӧ
	virtual void OnRspUserLogin(CThostFtdcRspUserLoginField *pRspUserLogin, CThostFtdcRspInfoField *pRspInfo, int nRequestID, bool bIsLast);
	///�ǳ�������Ӧ
	virtual void OnRspUserLogout(CThostFtdcUserLogoutField *pUserLogout, CThostFtdcRspInfoField *pRspInfo, int nRequestID, bool bIsLast);
	///��������Ӧ��
	virtual void OnRspSubMarketData(CThostFtdcSpecificInstrumentField *pSpecificInstrument, CThostFtdcRspInfoField *pRspInfo, int nRequestID, bool bIsLast);
	///�������֪ͨ
	virtual void OnRtnDepthMarketData(CThostFtdcDepthMarketDataField *pDepthMarketData);

	///ȡ����������Ӧ��
	virtual void OnRspUnSubMarketData(CThostFtdcSpecificInstrumentField *pSpecificInstrument, CThostFtdcRspInfoField *pRspInfo, int nRequestID, bool bIsLast);


public:
	//����ģʽ��1������һ��MdClient
	static napi_ref Singleton;

	static napi_status Init(napi_env env);
	static napi_status NewInstance(napi_env env,napi_value arg,napi_value* instance);
	static void Destructor(napi_env env, void* nativeObject, void* finalize_hint);
	static napi_value New(napi_env env, napi_callback_info info);

	//md ��¶�ӿ�api
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