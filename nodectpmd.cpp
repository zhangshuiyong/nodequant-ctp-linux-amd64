#include <cstring>
#include <iconv.h>
#include "common.h"
#include "nodectpmd.h"

using namespace std;

napi_ref CTPMarketDataClient::Singleton = nullptr;
napi_ref CTPMarketDataClient::prototype;
int CTPMarketDataClient::requestID;
map<string, int> CTPMarketDataClient::eventName_map;
map<int, napi_ref> CTPMarketDataClient::callback_map;

//------------------libuv channel���� ���߳�֪ͨ���߳�uv_async_send------>���߳�MainThreadCallback

CTPMarketDataClient::CTPMarketDataClient() : env_(nullptr), wrapper_(nullptr)
{
	//channel��data�̶�����, Ϊ��ǰʵ��
	channel.data = this;
	//��libuvͨ��
	uv_async_init(uv_default_loop(), &channel, (uv_async_cb)CTPMarketDataClient::MainThreadCallback);
	//�������
	uv_mutex_init(&eventQueueMutex);
}

//���߳�
void CTPMarketDataClient::queueEvent(OnEventCbRtnField* event)
{

	//1.����һ�����߳�On_xxx������Event�����У��ܿ�
	//2.�����������ʱ����������̻߳Ῠ����ctp����Ῠ��

	uv_mutex_lock(&eventQueueMutex);
	//дeventQueue
	eventQueue.push_back(event);
	uv_mutex_unlock(&eventQueueMutex);

	//���߳������̷߳���֪ͨ�������¼������е�events
	//uv_async_send�������̰߳�ȫ��
	uv_async_send(&channel);
}

//���߳���Ӧ
void CTPMarketDataClient::MainThreadCallback(uv_async_t* channel) {
	CTPMarketDataClient *marketDataClient = (CTPMarketDataClient*)channel->data;

	marketDataClient->processEventQueue();
}

//���߳�
void CTPMarketDataClient::processEventQueue() {
	//1.1.�¼������е��¼���ȫ��ִ�У�����������ִ�к���Ҫ��ʱ�䣡����

	//1.2.���ٺ�ʱ,ʹ�ö������棡�����¼������е��¼��󣬾������ͷ���

	uv_mutex_lock(&eventQueueMutex);

	vector<OnEventCbRtnField*> mainThreadEventList;
	//��eventQueue
	mainThreadEventList.swap(eventQueue);

	uv_mutex_unlock(&eventQueueMutex);

	for (int i = 0, size = mainThreadEventList.size(); i < size; i++) {
		process_event(mainThreadEventList[i]);
	}
}

CTPMarketDataClient::~CTPMarketDataClient()
{

	//��Ҫ�ٵ���һ�Σ�ȷ��ִ�������¼�
	processEventQueue();

	if (Api)
	{
		//�˳���,�ͷ�CTP��Api���̣߳�Spi�߳�Ҳ�ᱻ�ͷţ�
		Api->Release();
		Api = NULL;
	}

	//���ٺ��ٵ�ǰʵ�����¼���Ӧcallback
	map<int, napi_ref>::iterator callback_it = callback_map.begin();
	while (callback_it != callback_map.end()) {
		napi_ref callback = callback_it->second;
		napi_delete_reference(env_, callback);

		callback_it++;
	}

	callback_map.clear();


	//�����env_������������
	napi_delete_reference(env_, wrapper_);

	//�ر�libuvͨ��
	uv_close((uv_handle_t*)&channel, (uv_close_cb)CTPMarketDataClient::ChannelClosedCallback);
	//�����¼����еĻ�����
	uv_mutex_destroy(&eventQueueMutex);

	//�ͷź���ģʽ
	napi_delete_reference(env_, Singleton);
	Singleton = nullptr;

	//�������٣�����Ҫ�������prototype�������ã��������������
	//napi_delete_reference(env_, prototype);
}

void CTPMarketDataClient::ChannelClosedCallback(uv_async_t* pChannel) {
	
}

//------------------libuv channel���� ���߳�֪ͨ���߳�uv_async_send------>���߳�MainThreadCallback

void CTPMarketDataClient::Destructor(napi_env env,
	void* nativeObject,
	void* /*finalize_hint*/) {
	CTPMarketDataClient* obj = static_cast<CTPMarketDataClient*>(nativeObject);
	delete obj;
}

//������ľ�̬�� static prototype����
napi_status CTPMarketDataClient::Init(napi_env env)
{
	//��ʼ��static����eventName_map,�涨���е��¼���������δ������¼�
	if (eventName_map.size() == 0)
		initEventNameMap();

	napi_status status;
	//���е�Js������Native�㶼��static�����ĺ�����napi_callback��������
	//��Ϊc++������Ҫ��Js�����Wrap(��������)
	//������Js�㲻�ܿ����ͷ���c++������κ�����(���л�˽�л򷽷�)
	//ֻ�ܰ�c++���static�������ɷ��ʱ�������˽�����ԣ�����Ϊnapi_callback����
	//����ΪJs�㺯�����Ұ󶨵�c++����static��prototype�����������
	//ͨ��Js�������prototype���������ʱ�Wrap��ԭ��c++����
	//����myObject.on()
	//1.on��һ��Js�㺯��
	//2.on��MyObject���е�static����
	//3.ֻ��MyObject���ʵ��myObject������Ϊon Js������call����
	//4.����1,2,3,on Js������v8ִ��
	napi_property_descriptor properties[] = {
		DECLARE_NAPI_PROPERTY("on", on),
		DECLARE_NAPI_PROPERTY("connect", connect),
		DECLARE_NAPI_PROPERTY("login", login),
		DECLARE_NAPI_PROPERTY("subscribeMarketData", subscribeMarketData),
		DECLARE_NAPI_PROPERTY("unSubscribeMarketData", unSubscribeMarketData),
		DECLARE_NAPI_PROPERTY("logout", logout),
		DECLARE_NAPI_PROPERTY("getTradingDay", getTradingDay)
	};

	//����Js����prototype
	napi_value _proto_;
	//1.����prototype Js����
	//2.����New����ΪJs�㺯��,��������
	//3.��prototype�����constructor����ָ��New����
	//4.��prototype����󶨶������
	status = napi_define_class(
		env, "CTPMarketDataClient", NAPI_AUTO_LENGTH, New, nullptr, 
		sizeof(properties) / sizeof(*properties), properties, &_proto_);
	if (status != napi_ok) return status;

	//��ֹ���٣�Ϊprototype����ref����פ�浽���̵ľ�̬����
	status = napi_create_reference(env, _proto_, 1, &prototype);
	if (status != napi_ok) return status;

	return napi_ok;
}

void CTPMarketDataClient::initEventNameMap() {
	eventName_map["FrontConnected"] = T_On_FrontConnected;
	eventName_map["FrontDisconnected"] = T_On_FrontDisconnected;
	eventName_map["RspError"] = T_On_RspError;
	eventName_map["RspUserLogin"] = T_On_RspUserLogin;
	eventName_map["RspSubMarketData"] = T_On_RspSubMarketData;

	eventName_map["RtnDepthMarketData"] = T_On_RtnDepthMarketData;
	eventName_map["RspUnSubMarketData"] = T_On_RspUnSubMarketData;
	eventName_map["RspUserLogout"] = T_On_RspUserLogout;


	callback_map[T_On_FrontConnected] = nullptr;
	callback_map[T_On_FrontDisconnected] = nullptr;
	callback_map[T_On_RspError] = nullptr;
	callback_map[T_On_RspUserLogin] = nullptr;
	callback_map[T_On_RspSubMarketData] = nullptr;
	callback_map[T_On_RtnDepthMarketData] = nullptr;
	callback_map[T_On_RspUnSubMarketData] = nullptr;
	callback_map[T_On_RspUserLogout] = nullptr;
}

napi_status CTPMarketDataClient::NewInstance(napi_env env,napi_value arg,napi_value* instance) {
	napi_status status;

	napi_value _proto_;
	status = napi_get_reference_value(env, prototype, &_proto_);
	if (status != napi_ok) return status;

	status = napi_new_instance(env, _proto_, 0, nullptr, instance);
	if (status != napi_ok) return status;

	return napi_ok;
}
napi_value CTPMarketDataClient::New(napi_env env, napi_callback_info info)
{
	napi_value _this;
	NAPI_CALL(env, napi_get_cb_info(env, info, nullptr, nullptr, &_this, nullptr));

	CTPMarketDataClient* obj = new CTPMarketDataClient();
	obj->env_ = env;

	NAPI_CALL(env, napi_wrap(env,
		_this,
		obj,
		CTPMarketDataClient::Destructor,
		nullptr,  /* finalize_hint */
		&obj->wrapper_));

	return _this;
}

napi_value CTPMarketDataClient::on(napi_env env, napi_callback_info info)
{
	size_t argc = 2;
	napi_value args[2];
	napi_value _this;
	NAPI_CALL(env, napi_get_cb_info(env, info, &argc, args, &_this, nullptr));

	if (argc != 2) {
		napi_value msg;
		napi_create_string_utf8(env,
			"Wrong number of arguments.Right Format:on(string:eventName, function:eventCallBack)",
			NAPI_AUTO_LENGTH, &msg);
		napi_value err;
		napi_create_error(env, NULL, msg, &err);
		napi_fatal_exception(env, err);
		return NULL;
	}

	napi_valuetype eventNameType;
	NAPI_CALL(env, napi_typeof(env, args[0], &eventNameType));

	napi_valuetype eventCallbackType;
	NAPI_CALL(env, napi_typeof(env, args[1], &eventCallbackType));

	if (eventNameType != napi_string && eventCallbackType != napi_function) {
		napi_value msg;
		napi_create_string_utf8(env,
			"Parameter Type Error,Right Format:on(string:eventName, function:eventCallBack)",
			NAPI_AUTO_LENGTH, &msg);
		napi_value err;
		napi_create_error(env, NULL, msg, &err);
		napi_fatal_exception(env, err);
		return NULL;
	}

	char buffer[128];
	size_t buffer_size = 128;
	size_t copied;

	NAPI_CALL(env,
		napi_get_value_string_utf8(env, args[0], buffer, buffer_size, &copied));

	string eventName= buffer;
	
	map<string, int>::iterator eventItem = eventName_map.find(eventName);

	if (eventItem == eventName_map.end()) {
		napi_value msg;
		napi_create_string_utf8(env,
			"NodeQuant has no register this event",
			NAPI_AUTO_LENGTH, &msg);
		napi_value err;
		napi_create_error(env, NULL, msg, &err);
		napi_fatal_exception(env, err);
		return NULL;
	}

	CTPMarketDataClient* marketDataClient;
	NAPI_CALL(env, napi_unwrap(env, _this, reinterpret_cast<void**>(&marketDataClient)));

	//�Ѿ�������,���ٸ�����Ӧ����
	if (nullptr != callback_map[eventItem->second]) {
		return NULL;
	}

	NAPI_CALL(env, napi_create_reference(env, args[1], 1, &callback_map[eventItem->second]));

	return NULL;
}

//���ӽ�����ǰ̨
//���ӳɹ�ʱ��onFrontConnected�ص�����
//�Ͽ�����ʱ��onFrontDisconnected�ص�����
//Ӧ�����onRspError�ص�����
napi_value CTPMarketDataClient::connect(napi_env env, napi_callback_info info)
{
	size_t argc = 2;
	napi_value args[2];
	napi_value _this;
	NAPI_CALL(env, napi_get_cb_info(env, info, &argc, args, &_this, nullptr));

	if (argc != 2) {
		napi_value msg;
		napi_create_string_utf8(env,
			"Wrong number of arguments.Right Format:connect(String:address, String:mdFlowPath)",
			NAPI_AUTO_LENGTH, &msg);
		napi_value err;
		napi_create_error(env, NULL, msg, &err);
		napi_fatal_exception(env, err);
		return NULL;
	}

	napi_valuetype addressType;
	NAPI_CALL(env, napi_typeof(env, args[0], &addressType));

	napi_valuetype mdFlowPathType;
	NAPI_CALL(env, napi_typeof(env, args[1], &mdFlowPathType));

	if (addressType != napi_string || mdFlowPathType != napi_string) {
		napi_value msg;
		napi_create_string_utf8(env,
			"Parameter Type Error,Right Format:connect(String:address, String:mdFlowPath)",
			NAPI_AUTO_LENGTH, &msg);
		napi_value err;
		napi_create_error(env, NULL, msg, &err);
		napi_fatal_exception(env, err);
		return NULL;
	}


	ConnectField req;
	memset(&req, 0, sizeof(req));

	size_t buffer_size = 200;
	size_t copied;

	NAPI_CALL(env,
		napi_get_value_string_utf8(env, args[0], req.front_addr, buffer_size, &copied));

	buffer_size = 400;
	NAPI_CALL(env,
		napi_get_value_string_utf8(env, args[1], req.flowPath, buffer_size, &copied));


	CTPMarketDataClient* marketDataClient;
	NAPI_CALL(env, napi_unwrap(env, _this, reinterpret_cast<void**>(&marketDataClient)));

	//Js���߳�����ŵ���
	requestID++;
	//c++ void*����ָ��
	int nResult = marketDataClient->invoke(&req, T_CONNECT_RE, requestID);
	
	napi_value result;
	NAPI_CALL(env, napi_create_int32(env, nResult, &result));

	return result;
}

napi_value CTPMarketDataClient::getTradingDay(napi_env env, napi_callback_info info)
{
	size_t argc = 0;
	napi_value _this;
	NAPI_CALL(env, napi_get_cb_info(env, info, &argc, nullptr, &_this, nullptr));

	if (argc != 0) {
		napi_value msg;
		napi_create_string_utf8(env, 
			"Wrong number of arguments.Right Format: getTradingDay()", 
			NAPI_AUTO_LENGTH, &msg);
		napi_value err;
		napi_create_error(env, NULL, msg, &err);
		napi_fatal_exception(env, err);
		return NULL;
	}

	CTPMarketDataClient* marketDataClient;
	NAPI_CALL(env, napi_unwrap(env, _this, reinterpret_cast<void**>(&marketDataClient)));


	if (marketDataClient->Api) {

		napi_value result;
		string day = marketDataClient->Api->GetTradingDay();
		NAPI_CALL(env, napi_create_string_utf8(env, day.c_str(), day.length(), &result));

		return result;
	}else {
		return NULL;
	}
}

napi_value CTPMarketDataClient::login(napi_env env, napi_callback_info info)
{
	size_t argc = 4;
	napi_value args[4];
	napi_value _this;
	NAPI_CALL(env, napi_get_cb_info(env, info, &argc, args, &_this, nullptr));

	if (argc != 4) {
		napi_value msg;
		napi_create_string_utf8(env, 
			"Wrong number of arguments.Right Format: login(String:userID, String:password, String::brokerID,String::userProductInfo)",
			NAPI_AUTO_LENGTH, &msg);
		napi_value err;
		napi_create_error(env,NULL,msg,&err);
		napi_fatal_exception(env, err);
		return NULL;
	}
	
	
	napi_valuetype userIDType;
	NAPI_CALL(env, napi_typeof(env, args[0], &userIDType));

	napi_valuetype passwordType;
	NAPI_CALL(env, napi_typeof(env, args[1], &passwordType));

	napi_valuetype brokerIDType;
	NAPI_CALL(env, napi_typeof(env, args[2], &brokerIDType));

	napi_valuetype userProductInfoType;
	NAPI_CALL(env, napi_typeof(env, args[3], &userProductInfoType));


	if (userIDType != napi_string
		|| passwordType != napi_string
		|| brokerIDType != napi_string
		|| userProductInfoType != napi_string) {
		napi_value msg;
		napi_create_string_utf8(env, 
			"Parameter Type Error,Right Format:login(String:userID, String:password, String::brokerID,String::userProductInfo)",
			NAPI_AUTO_LENGTH, &msg);
		napi_value err;
		napi_create_error(env, NULL, msg, &err);
		napi_fatal_exception(env, err);
		return NULL;
	}

	//_completed��ɺ�������Ҫ����
	CThostFtdcReqUserLoginField req;
	memset(&req, 0, sizeof(req));

	//UserID 16�ַ�
	size_t buffer_size = 16;
	size_t copied;

	NAPI_CALL(env,
		napi_get_value_string_utf8(env, args[0], req.UserID, buffer_size, &copied));
	
	//Password 41�ַ�
	buffer_size = 41;
	NAPI_CALL(env,
		napi_get_value_string_utf8(env, args[1], req.Password, buffer_size, &copied));

	//BrokerID 11�ַ�
	buffer_size = 11;
	NAPI_CALL(env,
		napi_get_value_string_utf8(env, args[2], req.BrokerID, buffer_size, &copied));


	//UserProductInfo 11�ַ�
	buffer_size = 11;
	NAPI_CALL(env,
		napi_get_value_string_utf8(env, args[3], req.UserProductInfo, buffer_size, &copied));


	CTPMarketDataClient* marketDataClient;
	NAPI_CALL(env, napi_unwrap(env, _this, reinterpret_cast<void**>(&marketDataClient)));

	//Js���߳�����ŵ���
	requestID++;
	//c++ void*����ָ��
	int nResult = marketDataClient->invoke(&req, T_LOGIN_RE, requestID);

	napi_value result;
	NAPI_CALL(env, napi_create_int32(env, nResult, &result));

	return result;
}


napi_value CTPMarketDataClient::logout(napi_env env, napi_callback_info info)
{
	size_t argc = 2;
	napi_value args[2];
	napi_value _this;
	NAPI_CALL(env, napi_get_cb_info(env, info, &argc, args, &_this, nullptr));

	if (argc != 2) {
		napi_value msg;
		napi_create_string_utf8(env,
			"Wrong number of arguments.Right Format:logout(String:userID, String::brokerID)",
			NAPI_AUTO_LENGTH, &msg);
		napi_value err;
		napi_create_error(env, NULL, msg, &err);
		napi_fatal_exception(env, err);
		return NULL;
	}

	napi_valuetype userIDType;
	NAPI_CALL(env, napi_typeof(env, args[0], &userIDType));

	napi_valuetype brokerIDType;
	NAPI_CALL(env, napi_typeof(env, args[1], &brokerIDType));

	if (userIDType != napi_string
		|| brokerIDType != napi_string) {
		napi_value msg;
		napi_create_string_utf8(env,
			"Parameter Type Error,Right Format:logout(String:userID, String::brokerID)",
			NAPI_AUTO_LENGTH, &msg);
		napi_value err;
		napi_create_error(env, NULL, msg, &err);
		napi_fatal_exception(env, err);
		return NULL;
	}


	//_completed��ɺ�������Ҫ����
	CThostFtdcUserLogoutField req;
	memset(&req, 0, sizeof(req));

	//UserID 16�ַ�
	size_t buffer_size = 16;
	size_t copied;

	NAPI_CALL(env,
		napi_get_value_string_utf8(env, args[0], req.UserID, buffer_size, &copied));

	//BrokerID 11�ַ�
	buffer_size = 11;
	NAPI_CALL(env,
		napi_get_value_string_utf8(env, args[1], req.BrokerID, buffer_size, &copied));


	CTPMarketDataClient* marketDataClient;
	NAPI_CALL(env, napi_unwrap(env, _this, reinterpret_cast<void**>(&marketDataClient)));

	//Js���߳�����ŵ���
	requestID++;
	//c++ void*����ָ��
	int nResult = marketDataClient->invoke(&req, T_LOGOUT_RE, requestID);

	napi_value result;
	NAPI_CALL(env, napi_create_int32(env, nResult, &result));

	return result;
}

napi_value CTPMarketDataClient::subscribeMarketData(napi_env env, napi_callback_info info)
{
	size_t argc = 1;
	napi_value args[1];
	napi_value _this;
	NAPI_CALL(env, napi_get_cb_info(env, info, &argc, args, &_this, nullptr));

	if (argc != 1) {
		napi_value msg;
		napi_create_string_utf8(env,
			"Wrong number of arguments.Right Format: subscribeMarketData(String::contractName)",
			NAPI_AUTO_LENGTH, &msg);
		napi_value err;
		napi_create_error(env, NULL, msg, &err);
		napi_fatal_exception(env, err);
		return NULL;
	}

	napi_valuetype contractNameType;
	NAPI_CALL(env, napi_typeof(env, args[0], &contractNameType));

	if (contractNameType != napi_string) {
		napi_value msg;
		napi_create_string_utf8(env,
			"Parameter Type Error,Right Format: subscribeMarketData(String::contractName)",
			NAPI_AUTO_LENGTH, &msg);
		napi_value err;
		napi_create_error(env, NULL, msg, &err);
		napi_fatal_exception(env, err);
		return NULL;
	}

	char contractNameBuffer[128];
	size_t buffer_size = 128;
	size_t copied;

	NAPI_CALL(env,
		napi_get_value_string_utf8(env, args[0], contractNameBuffer, buffer_size, &copied));

	CTPMarketDataClient* marketDataClient;
	NAPI_CALL(env, napi_unwrap(env, _this, reinterpret_cast<void**>(&marketDataClient)));

	//Js���߳�����ŵ���
	requestID++;
	//c++ void*����ָ��
	int nResult = marketDataClient->invoke(contractNameBuffer, T_SUBSCRIBE_MARKET_DATA_RE, requestID);

	napi_value result;
	NAPI_CALL(env, napi_create_int32(env, nResult, &result));

	return result;
}

napi_value CTPMarketDataClient::unSubscribeMarketData(napi_env env, napi_callback_info info)
{
	size_t argc = 1;
	napi_value args[1];
	napi_value _this;
	NAPI_CALL(env, napi_get_cb_info(env, info, &argc, args, &_this, nullptr));

	if (argc != 1) {
		napi_value msg;
		napi_create_string_utf8(env,
			"Wrong number of arguments.Right Format: unSubscribeMarketData(String:contractName)",
			NAPI_AUTO_LENGTH, &msg);
		napi_value err;
		napi_create_error(env, NULL, msg, &err);
		napi_fatal_exception(env, err);
		return NULL;
	}

	napi_valuetype contractNameType;
	NAPI_CALL(env, napi_typeof(env, args[0], &contractNameType));

	if (contractNameType != napi_string) {
		napi_value msg;
		napi_create_string_utf8(env,
			"Parameter Type Error,Right Format: unSubscribeMarketData(String::contractName)",
			NAPI_AUTO_LENGTH, &msg);
		napi_value err;
		napi_create_error(env, NULL, msg, &err);
		napi_fatal_exception(env, err);
		return NULL;
	}

	char contractNameBuffer[128];
	size_t buffer_size = 128;
	size_t copied;

	NAPI_CALL(env,
		napi_get_value_string_utf8(env, args[0], contractNameBuffer, buffer_size, &copied));

	CTPMarketDataClient* marketDataClient;
	NAPI_CALL(env, napi_unwrap(env, _this, reinterpret_cast<void**>(&marketDataClient)));

	//Js���߳�����ŵ���
	requestID++;
	//c++ void*����ָ��
	int nResult = marketDataClient->invoke(contractNameBuffer, T_UNSUBSCRIBE_MARKET_DATA_RE, requestID);

	napi_value result;
	NAPI_CALL(env, napi_create_int32(env, nResult, &result));

	return result;
}


int CTPMarketDataClient::invoke(void* field, int fuctionType, int requestID)
{
	int nResult = 0;

	switch (fuctionType) {
		case T_CONNECT_RE:
		{
			ConnectField* _pConnectF = static_cast<ConnectField*>(field);
			this->Api = CThostFtdcMdApi::CreateFtdcMdApi(_pConnectF->flowPath);
			this->Api->RegisterSpi(this);
			this->Api->RegisterFront(_pConnectF->front_addr);
			this->Api->Init();

			break;
		}case T_LOGIN_RE:
		{
			CThostFtdcReqUserLoginField *_pReqUserLoginField = static_cast<CThostFtdcReqUserLoginField*>(field);
			nResult = this->Api->ReqUserLogin(_pReqUserLoginField, requestID);

			break;
		}case T_LOGOUT_RE:
		{
			CThostFtdcUserLogoutField* _pUserLogout = static_cast<CThostFtdcUserLogoutField*>(field);
			nResult = this->Api->ReqUserLogout(_pUserLogout, requestID);
			break;

		}case T_SUBSCRIBE_MARKET_DATA_RE:
		{
			char* contract = static_cast<char*>(field);
			char* arrayOfConracts[1] = { contract };
			nResult = this->Api->SubscribeMarketData(arrayOfConracts, 1);
			break;
		}case T_UNSUBSCRIBE_MARKET_DATA_RE:
		{
			string *pContractName = static_cast<string*>(field);
			char* contract = (char*)pContractName->c_str();
			char* arrayOfConracts[1] = { contract };
			nResult = this->Api->UnSubscribeMarketData(arrayOfConracts, 1);
			break;
		}
		default:
		{
			break;
		}
	}

	return nResult;
}

//////////////////////////////////////////////////////////�������Spi�߳���Ӧ����////////////////////////////////////////////////////////////////////////////////

void CTPMarketDataClient::OnFrontConnected()
{
	OnEventCbRtnField* field = new OnEventCbRtnField(); //������Ϻ���Ҫ����
	field->eFlag = T_On_FrontConnected;                 //FrontConnected
	queueEvent(field);                                  //�������ٺ�ָ�����
}

void CTPMarketDataClient::OnFrontDisconnected(int nReason) 
{
	OnEventCbRtnField* field = new OnEventCbRtnField();
	field->eFlag = T_On_FrontDisconnected;
	field->nReason = nReason;

	queueEvent(field);

}

void CTPMarketDataClient::on_invoke(int event_type, void* _stru, 
	CThostFtdcRspInfoField *pRspInfo_org, int nRequestID, bool bIsLast)
{
	CThostFtdcRspInfoField* _pRspInfo = NULL;
	if (pRspInfo_org) {
		_pRspInfo = new CThostFtdcRspInfoField();
		memcpy(_pRspInfo, pRspInfo_org, sizeof(CThostFtdcRspInfoField));
	}
	OnEventCbRtnField* field = new OnEventCbRtnField();
	
	field->eFlag = event_type;
	field->rtnField = _stru;
	field->rspInfo = (void*)_pRspInfo;
	field->nRequestID = nRequestID;
	field->bIsLast = bIsLast;
	
	queueEvent(field);

	//uv_thread_t id = uv_thread_self();
	//printf("thread id:%lu.\n", id);
}


void CTPMarketDataClient::OnRspError(CThostFtdcRspInfoField *pRspInfo, int nRequestID, bool bIsLast)
{
	CThostFtdcRspInfoField* _pRspInfo = NULL;
	if (pRspInfo) {
		_pRspInfo = new CThostFtdcRspInfoField();
		memcpy(_pRspInfo, pRspInfo, sizeof(CThostFtdcRspInfoField));
	}
	on_invoke(T_On_RspError, _pRspInfo, pRspInfo, nRequestID, bIsLast);
}

void  CTPMarketDataClient::OnRspUserLogin(CThostFtdcRspUserLoginField *pRspUserLogin, CThostFtdcRspInfoField *pRspInfo, int nRequestID, bool bIsLast)
{
	CThostFtdcRspUserLoginField* _pRspUserLogin = NULL;
	if (pRspUserLogin) {
		_pRspUserLogin = new CThostFtdcRspUserLoginField();
		memcpy(_pRspUserLogin, pRspUserLogin, sizeof(CThostFtdcRspUserLoginField));
	}
	on_invoke(T_On_RspUserLogin, _pRspUserLogin, pRspInfo, nRequestID, bIsLast);
}


void CTPMarketDataClient::OnRspUserLogout(CThostFtdcUserLogoutField *pUserLogout, CThostFtdcRspInfoField *pRspInfo, int nRequestID, bool bIsLast)
{
	CThostFtdcUserLogoutField* _pRspUserLogout = NULL;
	if (pUserLogout) {
		_pRspUserLogout = new CThostFtdcUserLogoutField();
		memcpy(_pRspUserLogout, pUserLogout, sizeof(CThostFtdcUserLogoutField));
	}
	
	on_invoke(T_On_RspUserLogout, _pRspUserLogout, pRspInfo, nRequestID, bIsLast);
}

void CTPMarketDataClient::OnRspSubMarketData(CThostFtdcSpecificInstrumentField *pSpecificInstrument, CThostFtdcRspInfoField *pRspInfo, int nRequestID, bool bIsLast)
{
	CThostFtdcSpecificInstrumentField* _pSpecificInstrument=NULL;
	if (pSpecificInstrument)
	{
		_pSpecificInstrument = new CThostFtdcSpecificInstrumentField();
		memcpy(_pSpecificInstrument, pSpecificInstrument, sizeof(CThostFtdcSpecificInstrumentField));
	}

	on_invoke(T_On_RspSubMarketData, _pSpecificInstrument, pRspInfo, nRequestID, bIsLast);
}


void CTPMarketDataClient::OnRtnDepthMarketData(CThostFtdcDepthMarketDataField *pDepthMarketData)
{
	CThostFtdcDepthMarketDataField* _pDepthMarketData=NULL;
	if (pDepthMarketData)
	{
		_pDepthMarketData = new CThostFtdcDepthMarketDataField();
		memcpy(_pDepthMarketData, pDepthMarketData, sizeof(CThostFtdcDepthMarketDataField));
	}

	on_invoke(T_On_RtnDepthMarketData, _pDepthMarketData, new CThostFtdcRspInfoField(), 0, 0);
}


void CTPMarketDataClient::OnRspUnSubMarketData(CThostFtdcSpecificInstrumentField *pSpecificInstrument, CThostFtdcRspInfoField *pRspInfo, int nRequestID, bool bIsLast)
{
	CThostFtdcSpecificInstrumentField* _pSpecificInstrument = NULL;
	if (pSpecificInstrument)
	{
		_pSpecificInstrument = new CThostFtdcSpecificInstrumentField();
		memcpy(_pSpecificInstrument, pSpecificInstrument, sizeof(CThostFtdcSpecificInstrumentField));
	}

	on_invoke(T_On_RspUnSubMarketData, _pSpecificInstrument, pRspInfo, nRequestID, bIsLast);
}


//��Ҫ��Js������ת���͵���Js��callback����
void  CTPMarketDataClient::process_event(OnEventCbRtnField* cbTrnField)
{
	napi_handle_scope handle_scope = nullptr;

	NAPI_CALL_RETURN_VOID(env_, napi_open_handle_scope(env_, &handle_scope));

	if (nullptr == callback_map[cbTrnField->eFlag])
		return;

	napi_value global;
	NAPI_CALL_RETURN_VOID(env_, napi_get_global(env_, &global));

	napi_value cb;
	NAPI_CALL_RETURN_VOID(env_, napi_get_reference_value(env_, callback_map[cbTrnField->eFlag], &cb));


	switch (cbTrnField->eFlag)
	{
	case T_On_FrontConnected:
	{
		NAPI_CALL_RETURN_VOID(env_, napi_call_function(env_, global, cb, 0, nullptr, NULL));
		break;
	}case T_On_FrontDisconnected:
	{
		napi_value argv[1];
		NAPI_CALL_RETURN_VOID(env_, napi_create_int32(env_, cbTrnField->nReason, argv));

		NAPI_CALL_RETURN_VOID(env_, napi_call_function(env_, global, cb, 1, argv, NULL));

		break;
	}case T_On_RspError:
	{
		napi_value argv[3];
		pkg_cb_rsperror(cbTrnField, argv);

		NAPI_CALL_RETURN_VOID(env_, napi_call_function(env_, global, cb, 3, argv, NULL));

		break;
	}case T_On_RspUserLogin:
	{
		napi_value argv[4];
		pkg_cb_userlogin(cbTrnField, argv);

		NAPI_CALL_RETURN_VOID(env_, napi_call_function(env_, global, cb, 4, argv, NULL));

		break;
	}case T_On_RspUserLogout:
	{
		napi_value argv[4];

		pkg_cb_userlogout(cbTrnField, argv);

		NAPI_CALL_RETURN_VOID(env_, napi_call_function(env_, global, cb, 4, argv, NULL));

		break;

	}case T_On_RspSubMarketData:
	{
		napi_value argv[4];
		pkg_cb_rspsubmarketdata(cbTrnField, argv);

		NAPI_CALL_RETURN_VOID(env_, napi_call_function(env_, global, cb, 4, argv, NULL));

		break;
	}case T_On_RtnDepthMarketData:
	{
		napi_value argv[1];
		pkg_cb_rtndepthmarketdata(cbTrnField, argv);

		NAPI_CALL_RETURN_VOID(env_, napi_call_function(env_, global, cb, 1, argv, NULL));

		break;
	}case T_On_RspUnSubMarketData:
	{
		napi_value argv[4];

		pkg_cb_unrspsubmarketdata(cbTrnField, argv);

		NAPI_CALL_RETURN_VOID(env_, napi_call_function(env_, global, cb, 4, argv, NULL));

		break;
	}default:
	{
		break;
	}
	}

	NAPI_CALL_RETURN_VOID(env_, napi_close_handle_scope(env_, handle_scope));

	if (cbTrnField->rtnField)
	{
		delete cbTrnField->rtnField;
		cbTrnField->rtnField = NULL;
	}

	if (cbTrnField->rspInfo)
	{
		delete cbTrnField->rspInfo;
		cbTrnField->rspInfo = NULL;
	}

	delete cbTrnField;
	cbTrnField = NULL;
}


void CTPMarketDataClient::pkg_cb_userlogout(OnEventCbRtnField* data, napi_value* cbArgs)
{
	CThostFtdcUserLogoutField* pRspUserLogout = static_cast<CThostFtdcUserLogoutField*>(data->rtnField);

	if (pRspUserLogout) {

		NAPI_CALL_RETURN_VOID(env_, napi_create_object(env_, cbArgs));

		napi_value brokerID;
		NAPI_CALL_RETURN_VOID(env_, napi_create_string_utf8(env_, pRspUserLogout->BrokerID, 
			NAPI_AUTO_LENGTH,&brokerID));

		napi_value userID;
		NAPI_CALL_RETURN_VOID(env_, napi_create_string_utf8(env_, pRspUserLogout->UserID,
			NAPI_AUTO_LENGTH, &userID));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs,"BrokerID", brokerID));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "UserID", userID));
	}
	else {

		NAPI_CALL_RETURN_VOID(env_, napi_get_undefined(env_, cbArgs));
	}

	CThostFtdcRspInfoField *pRspInfo = static_cast<CThostFtdcRspInfoField*>(data->rspInfo);
	
	pkg_rspinfo(pRspInfo, cbArgs + 1);

	NAPI_CALL_RETURN_VOID(env_, napi_create_int32(env_, data->nRequestID, (cbArgs+2)));

	NAPI_CALL_RETURN_VOID(env_, napi_get_boolean(env_, data->bIsLast, (cbArgs + 3)));
}

void CTPMarketDataClient::pkg_cb_unrspsubmarketdata(OnEventCbRtnField* data, napi_value* cbArgs)
{
	CThostFtdcSpecificInstrumentField *pSpecificInstrument = static_cast<CThostFtdcSpecificInstrumentField*>(data->rtnField);
	if (pSpecificInstrument) {

		NAPI_CALL_RETURN_VOID(env_, napi_create_string_utf8(env_, pSpecificInstrument->InstrumentID,
			NAPI_AUTO_LENGTH,
			cbArgs));
	}
	else {
		NAPI_CALL_RETURN_VOID(env_, napi_get_undefined(env_, cbArgs));
	}

	CThostFtdcRspInfoField *pRspInfo = static_cast<CThostFtdcRspInfoField*>(data->rspInfo);
	
	pkg_rspinfo(pRspInfo, (cbArgs + 1));

	NAPI_CALL_RETURN_VOID(env_, napi_create_int32(env_, data->nRequestID, (cbArgs + 2)));

	NAPI_CALL_RETURN_VOID(env_, napi_get_boolean(env_, data->bIsLast, (cbArgs + 3)));

}

void CTPMarketDataClient::pkg_cb_rtndepthmarketdata(OnEventCbRtnField* data, napi_value*cbArgs)
{
	CThostFtdcDepthMarketDataField *pDepthMarketData = static_cast<CThostFtdcDepthMarketDataField*>(data->rtnField);
	if (pDepthMarketData)
	{

		NAPI_CALL_RETURN_VOID(env_, napi_create_object(env_, cbArgs));

		napi_value TradingDay;
		napi_value InstrumentID;
		napi_value ExchangeID; 
		napi_value ExchangeInstID;
		napi_value LastPrice;
		napi_value PreSettlementPrice;
		napi_value PreClosePrice;
		napi_value PreOpenInterest;
		napi_value OpenPrice;
		napi_value HighestPrice;
		napi_value LowestPrice;
		napi_value Volume;
		napi_value Turnover;
		napi_value OpenInterest;
		napi_value ClosePrice;
		napi_value SettlementPrice;
		napi_value UpperLimitPrice;
		napi_value LowerLimitPrice;
		napi_value PreDelta;
		napi_value CurrDelta;
		napi_value UpdateTime;
		napi_value UpdateMillisec;
		napi_value BidPrice1;
		napi_value BidVolume1;
		napi_value AskPrice1;
		napi_value AskVolume1;
		napi_value BidPrice2;
		napi_value BidVolume2;
		napi_value AskPrice2;
		napi_value AskVolume2;
		napi_value BidPrice3;
		napi_value BidVolume3;
		napi_value AskPrice3;
		napi_value AskVolume3;
		napi_value BidPrice4;
		napi_value BidVolume4;
		napi_value AskPrice4;
		napi_value AskVolume4;
		napi_value BidPrice5;
		napi_value BidVolume5;
		napi_value AskPrice5;
		napi_value AskVolume5;
		napi_value AveragePrice;
		napi_value ActionDay;


		NAPI_CALL_RETURN_VOID(env_, napi_create_string_utf8(env_, pDepthMarketData->TradingDay,
			NAPI_AUTO_LENGTH, &TradingDay));

		NAPI_CALL_RETURN_VOID(env_, napi_create_string_utf8(env_, pDepthMarketData->InstrumentID,
			NAPI_AUTO_LENGTH, &InstrumentID));

		NAPI_CALL_RETURN_VOID(env_, napi_create_string_utf8(env_, pDepthMarketData->ExchangeID,
			NAPI_AUTO_LENGTH, &ExchangeID));

		NAPI_CALL_RETURN_VOID(env_, napi_create_string_utf8(env_, pDepthMarketData->ExchangeInstID,
			NAPI_AUTO_LENGTH, &ExchangeInstID));

		NAPI_CALL_RETURN_VOID(env_, napi_create_double(env_, pDepthMarketData->LastPrice, &LastPrice));
		NAPI_CALL_RETURN_VOID(env_, napi_create_double(env_, pDepthMarketData->PreSettlementPrice, &PreSettlementPrice));
		NAPI_CALL_RETURN_VOID(env_, napi_create_double(env_, pDepthMarketData->PreClosePrice, &PreClosePrice));
		NAPI_CALL_RETURN_VOID(env_, napi_create_double(env_, pDepthMarketData->PreOpenInterest, &PreOpenInterest));
		NAPI_CALL_RETURN_VOID(env_, napi_create_double(env_, pDepthMarketData->OpenPrice, &OpenPrice));
		NAPI_CALL_RETURN_VOID(env_, napi_create_double(env_, pDepthMarketData->HighestPrice, &HighestPrice));
		NAPI_CALL_RETURN_VOID(env_, napi_create_double(env_, pDepthMarketData->LowestPrice, &LowestPrice));
		NAPI_CALL_RETURN_VOID(env_, napi_create_int64(env_, pDepthMarketData->Volume, &Volume));
		NAPI_CALL_RETURN_VOID(env_, napi_create_double(env_, pDepthMarketData->Turnover, &Turnover));
		NAPI_CALL_RETURN_VOID(env_, napi_create_double(env_, pDepthMarketData->OpenInterest, &OpenInterest));
		NAPI_CALL_RETURN_VOID(env_, napi_create_double(env_, pDepthMarketData->ClosePrice, &ClosePrice));
		NAPI_CALL_RETURN_VOID(env_, napi_create_double(env_, pDepthMarketData->SettlementPrice, &SettlementPrice));
		NAPI_CALL_RETURN_VOID(env_, napi_create_double(env_, pDepthMarketData->UpperLimitPrice, &UpperLimitPrice));
		NAPI_CALL_RETURN_VOID(env_, napi_create_double(env_, pDepthMarketData->LowerLimitPrice, &LowerLimitPrice));
		NAPI_CALL_RETURN_VOID(env_, napi_create_double(env_, pDepthMarketData->PreDelta, &PreDelta));
		NAPI_CALL_RETURN_VOID(env_, napi_create_double(env_, pDepthMarketData->CurrDelta, &CurrDelta));

		NAPI_CALL_RETURN_VOID(env_, napi_create_string_utf8(env_, pDepthMarketData->UpdateTime,
			NAPI_AUTO_LENGTH, &UpdateTime));

		NAPI_CALL_RETURN_VOID(env_, napi_create_int64(env_, pDepthMarketData->UpdateMillisec, &UpdateMillisec));
		NAPI_CALL_RETURN_VOID(env_, napi_create_double(env_, pDepthMarketData->BidPrice1, &BidPrice1));
		NAPI_CALL_RETURN_VOID(env_, napi_create_int64(env_, pDepthMarketData->BidVolume1, &BidVolume1));
		NAPI_CALL_RETURN_VOID(env_, napi_create_double(env_, pDepthMarketData->AskPrice1, &AskPrice1));
		NAPI_CALL_RETURN_VOID(env_, napi_create_int64(env_, pDepthMarketData->AskVolume1, &AskVolume1));
		NAPI_CALL_RETURN_VOID(env_, napi_create_double(env_, pDepthMarketData->BidPrice2, &BidPrice2));
		NAPI_CALL_RETURN_VOID(env_, napi_create_int64(env_, pDepthMarketData->BidVolume2, &BidVolume2));
		NAPI_CALL_RETURN_VOID(env_, napi_create_double(env_, pDepthMarketData->AskPrice2, &AskPrice2));
		NAPI_CALL_RETURN_VOID(env_, napi_create_int64(env_, pDepthMarketData->AskVolume2, &AskVolume2));
		NAPI_CALL_RETURN_VOID(env_, napi_create_double(env_, pDepthMarketData->BidPrice3, &BidPrice3));
		NAPI_CALL_RETURN_VOID(env_, napi_create_int64(env_, pDepthMarketData->BidVolume3, &BidVolume3));
		NAPI_CALL_RETURN_VOID(env_, napi_create_double(env_, pDepthMarketData->AskPrice3, &AskPrice3));
		NAPI_CALL_RETURN_VOID(env_, napi_create_int64(env_, pDepthMarketData->AskVolume3, &AskVolume3));
		NAPI_CALL_RETURN_VOID(env_, napi_create_double(env_, pDepthMarketData->BidPrice4, &BidPrice4));
		NAPI_CALL_RETURN_VOID(env_, napi_create_int64(env_, pDepthMarketData->BidVolume4, &BidVolume4));
		NAPI_CALL_RETURN_VOID(env_, napi_create_double(env_, pDepthMarketData->AskPrice4, &AskPrice4));
		NAPI_CALL_RETURN_VOID(env_, napi_create_int64(env_, pDepthMarketData->AskVolume4, &AskVolume4));
		NAPI_CALL_RETURN_VOID(env_, napi_create_double(env_, pDepthMarketData->BidPrice5, &BidPrice5));
		NAPI_CALL_RETURN_VOID(env_, napi_create_int64(env_, pDepthMarketData->BidVolume5, &BidVolume5));
		NAPI_CALL_RETURN_VOID(env_, napi_create_double(env_, pDepthMarketData->AskPrice5, &AskPrice5));
		NAPI_CALL_RETURN_VOID(env_, napi_create_int64(env_, pDepthMarketData->AskVolume5, &AskVolume5));
		NAPI_CALL_RETURN_VOID(env_, napi_create_double(env_, pDepthMarketData->AveragePrice, &AveragePrice));

		NAPI_CALL_RETURN_VOID(env_, napi_create_string_utf8(env_, pDepthMarketData->ActionDay,
			NAPI_AUTO_LENGTH, &ActionDay));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "TradingDay", TradingDay));
		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "InstrumentID", InstrumentID));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "ExchangeID", ExchangeID));
		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "ExchangeInstID", ExchangeInstID));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "LastPrice", LastPrice));
		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "PreSettlementPrice", PreSettlementPrice));
		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "PreClosePrice", PreClosePrice));
		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "PreOpenInterest", PreOpenInterest));
		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "OpenPrice", OpenPrice));
		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "HighestPrice", HighestPrice));
		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "LowestPrice", LowestPrice));
		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "Volume", Volume));
		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "Turnover", Turnover));
		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "OpenInterest", OpenInterest));
		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "ClosePrice", ClosePrice));
		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "SettlementPrice", SettlementPrice));
		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "UpperLimitPrice", UpperLimitPrice));
		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "LowerLimitPrice", LowerLimitPrice));
		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "PreDelta", PreDelta));
		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "CurrDelta", CurrDelta));
		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "UpdateTime", UpdateTime));
		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "UpdateMillisec", UpdateMillisec));
		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "BidPrice1", BidPrice1));
		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "BidVolume1", BidVolume1));
		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "AskPrice1", AskPrice1));
		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "AskVolume1", AskVolume1));
		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "BidPrice2", BidPrice2));
		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "BidVolume2", BidVolume2));
		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "AskPrice2", AskPrice2));
		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "AskVolume2", AskVolume2));
		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "BidPrice3", BidPrice3));
		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "BidVolume3", BidVolume3));
		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "AskPrice3", AskPrice3));
		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "AskVolume3", AskVolume3));
		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "BidPrice4", BidPrice4));
		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "BidVolume4", BidVolume4));
		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "AskPrice4", AskPrice4));
		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "AskVolume4", AskVolume4));
		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "BidPrice5", BidPrice5));
		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "BidVolume5", BidVolume5));
		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "AskPrice5", AskPrice5));
		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "AskVolume5", AskVolume5));
		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "AveragePrice", AveragePrice));
		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "ActionDay", ActionDay));

	}
	else
	{
		NAPI_CALL_RETURN_VOID(env_, napi_get_undefined(env_, cbArgs));
	}

}

void CTPMarketDataClient::pkg_cb_rspsubmarketdata(OnEventCbRtnField* data, napi_value*cbArgs)
{
	CThostFtdcSpecificInstrumentField *pSpecificInstrument = static_cast<CThostFtdcSpecificInstrumentField*>(data->rtnField);
	if (pSpecificInstrument) {

		NAPI_CALL_RETURN_VOID(env_, napi_create_string_utf8(env_, pSpecificInstrument->InstrumentID,
			NAPI_AUTO_LENGTH,
			cbArgs));
	}
	else {
		NAPI_CALL_RETURN_VOID(env_, napi_get_undefined(env_, cbArgs));
	}

	CThostFtdcRspInfoField *pRspInfo = static_cast<CThostFtdcRspInfoField*>(data->rspInfo);
	
	pkg_rspinfo(pRspInfo, cbArgs + 1);

	NAPI_CALL_RETURN_VOID(env_, napi_create_int32(env_, data->nRequestID, (cbArgs + 2)));

	NAPI_CALL_RETURN_VOID(env_, napi_get_boolean(env_, data->bIsLast, (cbArgs + 3)));

}

void CTPMarketDataClient::pkg_cb_userlogin(OnEventCbRtnField* data, napi_value*cbArgs)
{
	CThostFtdcRspUserLoginField* pRspUserLogin = static_cast<CThostFtdcRspUserLoginField*>(data->rtnField);
	
	if (pRspUserLogin) {

		NAPI_CALL_RETURN_VOID(env_, napi_create_object(env_, cbArgs));

		napi_value TradingDay;
		napi_value LoginTime;
		napi_value BrokerID;
		napi_value UserID;
		napi_value SystemName;
		napi_value FrontID;
		napi_value SessionID;
		napi_value MaxOrderRef;
		napi_value SHFETime;
		napi_value DCETime;
		napi_value FFEXTime;
		napi_value INETime;

		NAPI_CALL_RETURN_VOID(env_, napi_create_string_utf8(env_, pRspUserLogin->TradingDay,
			NAPI_AUTO_LENGTH, &TradingDay));

		NAPI_CALL_RETURN_VOID(env_, napi_create_string_utf8(env_, pRspUserLogin->LoginTime,
			NAPI_AUTO_LENGTH, &LoginTime));

		NAPI_CALL_RETURN_VOID(env_, napi_create_string_utf8(env_, pRspUserLogin->BrokerID,
			NAPI_AUTO_LENGTH, &BrokerID));

		NAPI_CALL_RETURN_VOID(env_, napi_create_string_utf8(env_, pRspUserLogin->UserID,
			NAPI_AUTO_LENGTH, &UserID));

		NAPI_CALL_RETURN_VOID(env_, napi_create_string_utf8(env_, pRspUserLogin->SystemName,
			NAPI_AUTO_LENGTH, &SystemName));

		NAPI_CALL_RETURN_VOID(env_, napi_create_int32(env_, pRspUserLogin->FrontID, &FrontID));

		NAPI_CALL_RETURN_VOID(env_, napi_create_int32(env_, pRspUserLogin->SessionID, &SessionID));

		NAPI_CALL_RETURN_VOID(env_, napi_create_string_utf8(env_, pRspUserLogin->MaxOrderRef,
			NAPI_AUTO_LENGTH, &MaxOrderRef));

		NAPI_CALL_RETURN_VOID(env_, napi_create_string_utf8(env_, pRspUserLogin->SHFETime,
			NAPI_AUTO_LENGTH, &SHFETime));

		NAPI_CALL_RETURN_VOID(env_, napi_create_string_utf8(env_, pRspUserLogin->DCETime,
			NAPI_AUTO_LENGTH, &DCETime));

		NAPI_CALL_RETURN_VOID(env_, napi_create_string_utf8(env_, pRspUserLogin->FFEXTime,
			NAPI_AUTO_LENGTH, &FFEXTime));

		NAPI_CALL_RETURN_VOID(env_, napi_create_string_utf8(env_, pRspUserLogin->INETime,
			NAPI_AUTO_LENGTH, &INETime));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "TradingDay", TradingDay));
		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "LoginTime", LoginTime));
		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "BrokerID", BrokerID));
		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "UserID", UserID));
		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "SystemName", SystemName));
		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "FrontID", FrontID));
		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "SessionID", SessionID));
		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "MaxOrderRef", MaxOrderRef));
		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "SHFETime", SHFETime));
		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "DCETime", DCETime));
		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "FFEXTime", FFEXTime));
		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "INETime", INETime));
	}
	else {
		NAPI_CALL_RETURN_VOID(env_, napi_get_undefined(env_, cbArgs));
	}

	CThostFtdcRspInfoField *pRspInfo = static_cast<CThostFtdcRspInfoField*>(data->rspInfo);
	
	pkg_rspinfo(pRspInfo, (cbArgs + 1));

	NAPI_CALL_RETURN_VOID(env_, napi_create_int32(env_, data->nRequestID, (cbArgs + 2)));

	NAPI_CALL_RETURN_VOID(env_, napi_get_boolean(env_, data->bIsLast, (cbArgs + 3)));
}

void CTPMarketDataClient::pkg_cb_rsperror(OnEventCbRtnField* data, napi_value* cbArgs) {

	CThostFtdcRspInfoField *pRspInfo = static_cast<CThostFtdcRspInfoField*>(data->rspInfo);
	
	pkg_rspinfo(pRspInfo, cbArgs);

	NAPI_CALL_RETURN_VOID(env_, napi_create_int32(env_, data->nRequestID, (cbArgs + 1)));

	NAPI_CALL_RETURN_VOID(env_, napi_get_boolean(env_, data->bIsLast, (cbArgs + 2)));

}

void CTPMarketDataClient::pkg_rspinfo(CThostFtdcRspInfoField *pRspInfo, napi_value* cbArgs) {

	if (pRspInfo) {

		NAPI_CALL_RETURN_VOID(env_, napi_create_object(env_, cbArgs));

		napi_value ErrorID;
		NAPI_CALL_RETURN_VOID(env_, napi_create_int32(env_, pRspInfo->ErrorID, &ErrorID));
		
		//������Ϣ
		//typedef char TThostFtdcErrorMsgType[81];
		//���ַ�����char����->utf8�ַ���
		//string MsgStr(pRspInfo->ErrorMsg);
		string MsgUTF8 = CHString_To_UTF8(pRspInfo->ErrorMsg);
		napi_value ErrorMsg;
		NAPI_CALL_RETURN_VOID(env_, napi_create_string_utf8(env_, MsgUTF8.c_str(),
			NAPI_AUTO_LENGTH, &ErrorMsg));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "ErrorID", ErrorID));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "ErrorMsg", ErrorMsg));

	}
	else {
		NAPI_CALL_RETURN_VOID(env_, napi_get_undefined(env_, cbArgs));
	}

}

static int Gb2312ToUtf8(char *sOut, int iMaxOutLen, const char *sIn, int iInLen)  
{  
	char *pIn = (char *)sIn;  
	char *pOut = sOut;  
	size_t ret;  
	size_t iLeftLen=iMaxOutLen;  
	iconv_t cd;  

	cd = iconv_open("utf-8", "gb2312");  
	if (cd == (iconv_t) - 1)  
	{  
		return -1;  
	}  
	size_t iSrcLen=iInLen;  
	ret = iconv(cd, &pIn,&iSrcLen, &pOut,&iLeftLen);  
	if (ret == (size_t) - 1)  
	{  
		iconv_close(cd);  
		return -1;  
	}  

	iconv_close(cd);  

	return (iMaxOutLen - iLeftLen);  
}

string CTPMarketDataClient::CHString_To_UTF8(char* pszOri)
{
    char pszDst[255] = {0};  
      
	int iLen = Gb2312ToUtf8(pszDst, 50, pszOri, strlen(pszOri));
	
	std::string retStr(pszDst);
	
	return retStr;
}
