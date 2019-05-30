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
	CThostFtdcTraderApi* Api;

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


	///1.���ͻ����뽻�׺�̨������ͨ������ʱ����δ��¼ǰ�����÷��������á�
	virtual void OnFrontConnected();
	///2.���ͻ����뽻�׺�̨ͨ�����ӶϿ�ʱ���÷��������á���������������API���Զ��������ӣ��ͻ��˿ɲ�������
	///@param nReason ����ԭ��
	///        0x1001 4097 �����ʧ�� 
	///        0x1002 4098 ����дʧ��
	///        0x2001 8193 ����������ʱ
	///        0x2002 8194 ��������ʧ��
	///        0x2003 8195 �յ�������
	virtual void OnFrontDisconnected(int nReason);

	///3.����Ӧ��
	virtual void OnRspError(CThostFtdcRspInfoField *pRspInfo, int nRequestID, bool bIsLast);

	///4.�ͻ�����֤��Ӧ
	virtual void OnRspAuthenticate(CThostFtdcRspAuthenticateField *pRspAuthenticateField, CThostFtdcRspInfoField *pRspInfo, int nRequestID, bool bIsLast);

	///5.��¼������Ӧ
	virtual void OnRspUserLogin(CThostFtdcRspUserLoginField *pRspUserLogin, CThostFtdcRspInfoField *pRspInfo, int nRequestID, bool bIsLast);
	
	///�����ѯͶ���߽�������Ӧ
	virtual void OnRspQrySettlementInfo(CThostFtdcSettlementInfoField *pSettlementInfo, CThostFtdcRspInfoField *pRspInfo, int nRequestID, bool bIsLast);

	///6.Ͷ���߽�����ȷ����Ӧ
	virtual void OnRspSettlementInfoConfirm(CThostFtdcSettlementInfoConfirmField *pSettlementInfoConfirm, CThostFtdcRspInfoField *pRspInfo, int nRequestID, bool bIsLast);

	///7.�����ѯ��Լ��Ӧ
	virtual void OnRspQryInstrument(CThostFtdcInstrumentField *pInstrument, CThostFtdcRspInfoField *pRspInfo, int nRequestID, bool bIsLast);

	///8.�ǳ�������Ӧ
	virtual void OnRspUserLogout(CThostFtdcUserLogoutField *pUserLogout, CThostFtdcRspInfoField *pRspInfo, int nRequestID, bool bIsLast);
	///9.�����ѯͶ���ֲ߳���Ӧ
	virtual void OnRspQryInvestorPosition(CThostFtdcInvestorPositionField *pInvestorPosition, CThostFtdcRspInfoField *pRspInfo, int nRequestID, bool bIsLast);
	///10.�����ѯ�ʽ��˻���Ӧ
	virtual void OnRspQryTradingAccount(CThostFtdcTradingAccountField *pTradingAccount, CThostFtdcRspInfoField *pRspInfo, int nRequestID, bool bIsLast);
	///11.����¼��������Ӧ
	//���׺��Ķ��յ��Ľ������б������Ϸ��Լ�飬��������Ľ������뱨�ĺ�ͻ᷵�ظ�����ǰ��һ��
	//����������Ϣ�ı�����Ӧ���ģ�����ǰ���������ñ�����Ϣת���������նˡ�
	virtual void OnRspOrderInsert(CThostFtdcInputOrderField *pInputOrder, CThostFtdcRspInfoField *pRspInfo, int nRequestID, bool bIsLast);
	///12.����¼�����ر�
	//�˽ӿڽ��ڱ����� CTP �˾ܾ�ʱ�������������б���
	virtual void OnErrRtnOrderInsert(CThostFtdcInputOrderField *pInputOrder, CThostFtdcRspInfoField *pRspInfo);
	///13.����֪ͨ
	//���׺�������������ñ�����������뱨�ģ��ᱻ���ö��
	//1.���������� 2.���ܸñ���ʱ 3.�ñ����ɽ�ʱ 4.��������У��ʧ��OrderStatusMsg
	//����֪ͨ
	//�����������Ϊ����ָ��Ϸ���ͬ���᷵�ض�Ӧ��������״̬��OnRtnOrder����
	//���׺���ȷ���˳���ָ��ĺϷ��Ժ󣬽��ó���ָ���ύ����������ͬʱ���ض�Ӧ��������״̬����OnRtnOrder��
	virtual void OnRtnOrder(CThostFtdcOrderField *pOrder);
	///14.�ɽ�֪ͨ
	//�������б����ɽ�֮��һ�������ر���OnRtnOrder����һ���ɽ��ر���OnRtnTrade���ᱻ���͵��ͻ��ˣ������ر�
	//�б�����״̬Ϊ���ѳɽ�����������Ȼ����ͻ��˽��ɽ��ر���Ϊ�����ɽ��ı�־����Ϊ CTP �Ľ��׺�����
	//�յ� OnRtnTrade ֮��Ż���¸ñ�����״̬��
	virtual void OnRtnTrade(CThostFtdcTradeField *pTrade);

	///15.��������������Ӧ
	//������Ӧ�����׺��ķ��صĺ��д�����Ϣ�ĳ�����Ӧ
	virtual void OnRspOrderAction(CThostFtdcInputOrderActionField *pInputOrderAction, CThostFtdcRspInfoField *pRspInfo, int nRequestID, bool bIsLast);

	///16.������������ر�
	//���������ٴ���֤����ָ��ĺϷ��ԣ������������Ϊ��ָ��Ϸ������׺���ͨ���˺���ת�������������Ĵ���
	virtual void OnErrRtnOrderAction(CThostFtdcOrderActionField *pOrderAction, CThostFtdcRspInfoField *pRspInfo);

	//17.�������ʲ�ѯ����
	virtual void OnRspQryInstrumentCommissionRate(CThostFtdcInstrumentCommissionRateField *pInstrumentCommissionRate, CThostFtdcRspInfoField *pRspInfo, int nRequestID, bool bIsLast);
public:

	//����ģʽ��1������һ��TdClient
	static napi_ref Singleton;

	static napi_status Init(napi_env env);
	static napi_status NewInstance(napi_env env, napi_value arg, napi_value* instance);
	static void Destructor(napi_env env, void* nativeObject, void* finalize_hint);
	static napi_value New(napi_env env, napi_callback_info info);


	//td ��¶�ӿ�api
	static napi_value on(napi_env env, napi_callback_info info);
	static napi_value connect(napi_env env, napi_callback_info info);
	static napi_value getApiVersion(napi_env env, napi_callback_info info);
	static napi_value authenticate(napi_env env, napi_callback_info info);
	static napi_value login(napi_env env, napi_callback_info info);
	static napi_value querySettlementInfo(napi_env env, napi_callback_info info);
	static napi_value confirmSettlementInfo(napi_env env, napi_callback_info info);
	static napi_value queryInstrument(napi_env env, napi_callback_info info);
	static napi_value queryTradingAccount(napi_env env, napi_callback_info info);

	//��ѯ�ֲ֣����ܣ�
	//CTP ϵͳ���ֲ���ϸ��¼����Լ���ֲַ��򣬿������ڣ��������������������֡���֣����л���
	//�ֲֻ��ܼ�¼�У�
	//YdPosition ��ʾ��������ʱ�ֲ��������� ��ǰ�������������̬���ռ䲻���ſ�ƽ�ֶ��仯��
	//Position ��ʾ��ǰ�ֲ�����
	//TodayPosition ��ʾ���¿���
	//��ǰ��������� = ��Position - ��TodayPosition
	static napi_value queryInvestorPosition(napi_env env, napi_callback_info info);

	static napi_value sendOrder(napi_env env, napi_callback_info info);

	static napi_value cancelOrder(napi_env env, napi_callback_info info);

	static napi_value logout(napi_env env, napi_callback_info info);

	//��ѯ������
	static napi_value queryCommissionRate(napi_env env, napi_callback_info info);

	static string CHString_To_UTF8(char* str);
};
