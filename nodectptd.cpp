#include<stdio.h>
#include <cstring>
#include <iconv.h>
#include "common.h"
#include "nodectptd.h"

using namespace std;

napi_ref CTPTraderClient::Singleton = nullptr;
napi_ref CTPTraderClient::prototype;
int CTPTraderClient::requestID;
map<string, int> CTPTraderClient::eventName_map;
map<int, napi_ref> CTPTraderClient::callback_map;

//------------------libuv channel机制 子线程通知主线程uv_async_send------>主线程MainThreadCallback

CTPTraderClient::CTPTraderClient()
{
	//channel的data固定不变, 为当前实例
	channel.data = this;
	//打开libuv通道
	uv_async_init(uv_default_loop(), &channel, (uv_async_cb)CTPTraderClient::MainThreadCallback);
	//激活互斥锁
	uv_mutex_init(&eventQueueMutex);
}

//子线程
void CTPTraderClient::queueEvent(OnEventCbRtnField* event)
{

	//1.插入一个子线程On_xxx函数的Event到队列，很快
	//2.这里如果处理时间过长，子线程会卡死，ctp程序会卡死

	uv_mutex_lock(&eventQueueMutex);
	//写eventQueue
	eventQueue.push_back(event);
	uv_mutex_unlock(&eventQueueMutex);

	//子线程向主线程发送通知，处理事件队列中的events
	//uv_async_send函数是线程安全的
	uv_async_send(&channel);
}

//主线程响应
void CTPTraderClient::MainThreadCallback(uv_async_t* channel) {
	CTPTraderClient * traderClient = (CTPTraderClient*)channel->data;

	traderClient->processEventQueue();
}

//主线程
void CTPTraderClient::processEventQueue() {
	//1.1.事件队列中的事件，全部执行，这里在锁内执行函数要耗时间！！！

	//1.2.减少耗时,使用二级缓存！！！事件队列中的事件后，就立即释放锁

	uv_mutex_lock(&eventQueueMutex);

	vector<OnEventCbRtnField*> mainThreadEventList;
	//读eventQueue
	mainThreadEventList.swap(eventQueue);

	uv_mutex_unlock(&eventQueueMutex);

	for (int i = 0, size = mainThreadEventList.size(); i < size; i++) {
		process_event(mainThreadEventList[i]);
	}

}

CTPTraderClient::~CTPTraderClient()
{
	//需要再调用一次，确保执行所有事件
	processEventQueue();

	if (Api)
	{
		//退出后,释放CTP的Api主线程（Spi线程也会被释放）
		Api->Release();
		Api = NULL;
	}

	//销毁后再当前实例的事件相应callback
	map<int, napi_ref>::iterator callback_it = callback_map.begin();
	while (callback_it != callback_map.end()) {
		napi_ref callback = callback_it->second;
		napi_delete_reference(env_, callback);

		callback_it++;
	}

	callback_map.clear();


	//保存的env_在这里起作用
	napi_delete_reference(env_, wrapper_);

	//关闭libuv通道
	uv_close((uv_handle_t*)&channel, (uv_close_cb)CTPTraderClient::ChannelClosedCallback);
	//销毁事件队列的互斥锁
	uv_mutex_destroy(&eventQueueMutex);

	//释放后单例模式
	napi_delete_reference(env_, Singleton);
	Singleton = nullptr;

	//对象销毁，不需要销毁类的prototype对象引用，它跟随进程销毁
	//napi_delete_reference(env_, prototype);
}

void CTPTraderClient::ChannelClosedCallback(uv_async_t* pChannel) {

}

//------------------libuv channel机制 子线程通知主线程uv_async_send------>主线程MainThreadCallback

void CTPTraderClient::Destructor(napi_env env,
	void* nativeObject,
	void* /*finalize_hint*/) {
	CTPTraderClient* obj = static_cast<CTPTraderClient*>(nativeObject);
	delete obj;
}

napi_status CTPTraderClient::Init(napi_env env)
{

	//初始化static变量eventName_map,规定所有的事件，不允许未定义的事件
	if (eventName_map.size() == 0)
		initEventNameMap();

	napi_status status;
	napi_property_descriptor properties[] = {
		DECLARE_NAPI_PROPERTY("on", on),
		DECLARE_NAPI_PROPERTY("connect", connect),
		DECLARE_NAPI_PROPERTY("authenticate", authenticate),
		DECLARE_NAPI_PROPERTY("login", login),
		DECLARE_NAPI_PROPERTY("querySettlementInfo", querySettlementInfo),
		DECLARE_NAPI_PROPERTY("confirmSettlementInfo", confirmSettlementInfo),
		DECLARE_NAPI_PROPERTY("queryInstrument", queryInstrument),
		DECLARE_NAPI_PROPERTY("sendOrder", sendOrder),
		DECLARE_NAPI_PROPERTY("cancelOrder", cancelOrder),
		DECLARE_NAPI_PROPERTY("logout", logout),
		DECLARE_NAPI_PROPERTY("queryTradingAccount", queryTradingAccount),
		DECLARE_NAPI_PROPERTY("queryInvestorPosition", queryInvestorPosition),
		DECLARE_NAPI_PROPERTY("queryCommissionRate", queryCommissionRate),
	};

	//创建Js对象prototype
	napi_value _proto_;
	//1.创建prototype Js对象
	//2.创建New函数为Js层函数,并且命名
	//3.给prototype对象的constructor属性指向New函数
	//4.给prototype对象绑定多个属性
	status = napi_define_class(
		env, "CTPTraderClient", NAPI_AUTO_LENGTH, New, nullptr,
		sizeof(properties) / sizeof(*properties), properties, &_proto_);
	if (status != napi_ok) return status;

	//防止销毁，为prototype创建ref对象，驻存到进程的静态区中
	status = napi_create_reference(env, _proto_, 1, &prototype);
	if (status != napi_ok) return status;

	return napi_ok;
}

void CTPTraderClient::initEventNameMap()
{
	eventName_map["FrontConnected"] = T_On_FrontConnected;
	eventName_map["FrontDisconnected"] = T_On_FrontDisconnected;
	eventName_map["RspError"] = T_On_RspError;
	eventName_map["RspAuthenticate"] = T_On_RspAuthenticate;
	eventName_map["RspUserLogin"] = T_On_RspUserLogin;

	eventName_map["RspQrySettlementInfo"] = T_On_RspQrySettlementInfo;
	eventName_map["RspSettlementInfoConfirm"] = T_On_RspSettlementInfoConfirm;
	eventName_map["RspQryInstrument"] = T_On_RspQryInstrument;
	eventName_map["RspUserLogout"] = T_On_RspUserLogout;

	eventName_map["RspQryInvestorPosition"] = T_On_RspQryInvestorPosition;
	eventName_map["RspQryTradingAccount"] = T_On_RspQryTradingAccount;
	eventName_map["RspOrderInsert"] = T_On_RspOrderInsert;
	eventName_map["ErrRtnOrderInsert"] = T_On_ErrRtnOrderInsert;
	eventName_map["RtnOrder"] = T_On_RtnOrder;
	eventName_map["RtnTrade"] = T_On_RtnTrade;
	eventName_map["RspOrderAction"] = T_On_RspOrderAction;
	eventName_map["ErrRtnOrderAction"] = T_On_ErrRtnOrderAction;
	eventName_map["RspQryInstrumentCommissionRate"] = T_On_RspQryInstrumentCommissionRate;


	callback_map[T_On_FrontConnected] = nullptr;
	callback_map[T_On_FrontDisconnected] = nullptr;
	callback_map[T_On_RspError] = nullptr;
	callback_map[T_On_RspAuthenticate] = nullptr;
	callback_map[T_On_RspUserLogin] = nullptr;
	callback_map[T_On_RspQrySettlementInfo] = nullptr;
	callback_map[T_On_RspSettlementInfoConfirm] = nullptr;
	callback_map[T_On_RspQryInstrument] = nullptr;
	callback_map[T_On_RspUserLogout] = nullptr;
	callback_map[T_On_RspQryInvestorPosition] = nullptr;
	callback_map[T_On_RspQryTradingAccount] = nullptr;
	callback_map[T_On_RspOrderInsert] = nullptr;
	callback_map[T_On_ErrRtnOrderInsert] = nullptr;
	callback_map[T_On_RtnOrder] = nullptr;
	callback_map[T_On_RtnTrade] = nullptr;
	callback_map[T_On_RspOrderAction] = nullptr;
	callback_map[T_On_ErrRtnOrderAction] = nullptr;
	callback_map[T_On_RspQryInstrumentCommissionRate] = nullptr;
}

napi_status CTPTraderClient::NewInstance(napi_env env, napi_value arg, napi_value* instance) {
	napi_status status;

	napi_value _proto_;
	status = napi_get_reference_value(env, prototype, &_proto_);
	if (status != napi_ok) return status;

	status = napi_new_instance(env, _proto_, 0, nullptr, instance);
	if (status != napi_ok) return status;

	return napi_ok;
}

napi_value CTPTraderClient::New(napi_env env, napi_callback_info info)
{
	napi_value _this;
	NAPI_CALL(env, napi_get_cb_info(env, info, nullptr, nullptr, &_this, nullptr));

	CTPTraderClient* obj = new CTPTraderClient();
	obj->env_ = env;

	NAPI_CALL(env, napi_wrap(env,
		_this,
		obj,
		CTPTraderClient::Destructor,
		nullptr,  /* finalize_hint */
		&obj->wrapper_));

	return _this;
}

napi_value CTPTraderClient::on(napi_env env, napi_callback_info info)
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

	if (eventNameType != napi_string || eventCallbackType != napi_function) {
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

	string eventName = buffer;
	
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

	//已经设置了,不再更改响应函数
	if (nullptr != callback_map[eventItem->second]) {
		return NULL;
	}

	NAPI_CALL(env, napi_create_reference(env, args[1], 1, &callback_map[eventItem->second]));

	return NULL;
}

napi_value CTPTraderClient::connect(napi_env env, napi_callback_info info)
{
	size_t argc = 2;
	napi_value args[2];
	napi_value _this;
	NAPI_CALL(env, napi_get_cb_info(env, info, &argc, args, &_this, nullptr));

	if (argc != 2) {
		napi_value msg;
		napi_create_string_utf8(env,
			"Wrong number of arguments.Right Format:connect(String:address, String:traderFlowPath)",
			NAPI_AUTO_LENGTH, &msg);
		napi_value err;
		napi_create_error(env, NULL, msg, &err);
		napi_fatal_exception(env, err);
		return NULL;
	}

	napi_valuetype addressType;
	NAPI_CALL(env, napi_typeof(env, args[0], &addressType));

	napi_valuetype tdFlowPathType;
	NAPI_CALL(env, napi_typeof(env, args[1], &tdFlowPathType));

	if (addressType != napi_string || tdFlowPathType != napi_string) {
		napi_value msg;
		napi_create_string_utf8(env,
			"Parameter Type Error,Right Format:connect(String:address, String:traderFlowPath)",
			NAPI_AUTO_LENGTH, &msg);
		napi_value err;
		napi_create_error(env, NULL, msg, &err);
		napi_fatal_exception(env, err);
		return NULL;
	}

	//栈变量自动释放
	ConnectField req;
	memset(&req, 0, sizeof(req));

	size_t buffer_size = 200;
	size_t copied;

	NAPI_CALL(env,
		napi_get_value_string_utf8(env, args[0], req.front_addr, buffer_size, &copied));

	buffer_size = 400;
	NAPI_CALL(env,
		napi_get_value_string_utf8(env, args[1], req.flowPath, buffer_size, &copied));


	CTPTraderClient* traderClient;
	NAPI_CALL(env, napi_unwrap(env, _this, reinterpret_cast<void**>(&traderClient)));

	//Js主线程请求号递增
	requestID++;
	//c++ void*泛型指针
	int nResult = traderClient->invoke(&req, T_CONNECT_RE, requestID);

	napi_value result;
	NAPI_CALL(env, napi_create_int32(env, nResult, &result));

	return result;
}

napi_value CTPTraderClient::authenticate(napi_env env, napi_callback_info info)
{
	size_t argc = 4;
	napi_value args[4];
	napi_value _this;
	NAPI_CALL(env, napi_get_cb_info(env, info, &argc, args, &_this, nullptr));

	if (argc != 4) {
		napi_value msg;
		napi_create_string_utf8(env,
			"Wrong number of arguments.Right Format:authenticate(String:userID, String::brokerID, String:authCode,String:userProductInfo)",
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

	napi_valuetype authCodeType;
	NAPI_CALL(env, napi_typeof(env, args[2], &authCodeType));

	napi_valuetype userProductInfoType;
	NAPI_CALL(env, napi_typeof(env, args[3], &userProductInfoType));

	if (userIDType != napi_string ||
		brokerIDType != napi_string ||
		authCodeType != napi_string ||
		userProductInfoType != napi_string) {
		napi_value msg;
		napi_create_string_utf8(env,
			"Parameter Type Error,Right Format:authenticate(String:userID, String::brokerID, String:authCode,String:userProductInfo)",
			NAPI_AUTO_LENGTH, &msg);
		napi_value err;
		napi_create_error(env, NULL, msg, &err);
		napi_fatal_exception(env, err);
		return NULL;
	}

	CThostFtdcReqAuthenticateField req;
	memset(&req, 0, sizeof(req));

	//UserID 16字符
	size_t buffer_size = 16;
	size_t copied;

	NAPI_CALL(env,
		napi_get_value_string_utf8(env, args[0], req.UserID, buffer_size, &copied));

	//BrokerID 11字符
	buffer_size = 11;
	NAPI_CALL(env,
		napi_get_value_string_utf8(env, args[1], req.BrokerID, buffer_size, &copied));

	//AuthCode 17字符
	buffer_size = 17;
	NAPI_CALL(env,
		napi_get_value_string_utf8(env, args[2], req.AuthCode, buffer_size, &copied));

	//UserProductInfo 11字符
	buffer_size = 11;
	NAPI_CALL(env,
		napi_get_value_string_utf8(env, args[3], req.UserProductInfo, buffer_size, &copied));

	CTPTraderClient* traderClient;
	NAPI_CALL(env, napi_unwrap(env, _this, reinterpret_cast<void**>(&traderClient)));

	//Js主线程请求号递增
	requestID++;
	//c++ void*泛型指针
	int nResult = traderClient->invoke(&req, T_Authenticate_RE, requestID);

	napi_value result;
	NAPI_CALL(env, napi_create_int32(env, nResult, &result));

	return result;
}

napi_value  CTPTraderClient::login(napi_env env, napi_callback_info info)
{
	size_t argc = 4;
	napi_value args[4];
	napi_value _this;
	NAPI_CALL(env, napi_get_cb_info(env, info, &argc, args, &_this, nullptr));

	if (argc != 4) {
		napi_value msg;
		napi_create_string_utf8(env,
			"Wrong number of arguments.Right Format:login(String:userID, String:password, String::brokerID,String::userProductInfo)",
			NAPI_AUTO_LENGTH, &msg);
		napi_value err;
		napi_create_error(env, NULL, msg, &err);
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


	CThostFtdcReqUserLoginField req;
	memset(&req, 0, sizeof(req));

	//UserID 16字符
	size_t buffer_size = 16;
	size_t copied;

	NAPI_CALL(env,
		napi_get_value_string_utf8(env, args[0], req.UserID, buffer_size, &copied));

	//Password 41字符
	buffer_size = 41;
	NAPI_CALL(env,
		napi_get_value_string_utf8(env, args[1], req.Password, buffer_size, &copied));

	//BrokerID 11字符
	buffer_size = 11;
	NAPI_CALL(env,
		napi_get_value_string_utf8(env, args[2], req.BrokerID, buffer_size, &copied));


	//UserProductInfo 11字符
	buffer_size = 11;
	NAPI_CALL(env,
		napi_get_value_string_utf8(env, args[3], req.UserProductInfo, buffer_size, &copied));


	CTPTraderClient* traderClient;
	NAPI_CALL(env, napi_unwrap(env, _this, reinterpret_cast<void**>(&traderClient)));

	//Js主线程请求号递增
	requestID++;
	//c++ void*泛型指针
	int nResult = traderClient->invoke(&req, T_LOGIN_RE, requestID);

	napi_value result;
	NAPI_CALL(env, napi_create_int32(env, nResult, &result));

	return result;

}

napi_value CTPTraderClient::querySettlementInfo(napi_env env, napi_callback_info info)
{
	size_t argc = 2;
	napi_value args[2];
	napi_value _this;
	NAPI_CALL(env, napi_get_cb_info(env, info, &argc, args, &_this, nullptr));

	if (argc != 2) {
		napi_value msg;
		napi_create_string_utf8(env,
			"Wrong number of arguments.Right Format:querySettlementInfo(String:userID, String::brokerID)",
			NAPI_AUTO_LENGTH, &msg);
		napi_value err;
		napi_create_error(env, NULL, msg, &err);
		napi_fatal_exception(env, err);
		return NULL;
	}

	napi_valuetype investorIDType;
	NAPI_CALL(env, napi_typeof(env, args[0], &investorIDType));

	napi_valuetype brokerIDType;
	NAPI_CALL(env, napi_typeof(env, args[1], &brokerIDType));

	if (investorIDType != napi_string
		|| brokerIDType != napi_string) {
		napi_value msg;
		napi_create_string_utf8(env,
			"Parameter Type Error,Right Format:querySettlementInfo(String:userID, String::brokerID)",
			NAPI_AUTO_LENGTH, &msg);
		napi_value err;
		napi_create_error(env, NULL, msg, &err);
		napi_fatal_exception(env, err);
		return NULL;
	}


	CThostFtdcQrySettlementInfoField req;
	memset(&req, 0, sizeof(req));

	//InvestorID 13字符
	size_t buffer_size = 13;
	size_t copied;

	NAPI_CALL(env,
		napi_get_value_string_utf8(env, args[0], req.InvestorID, buffer_size, &copied));

	//BrokerID 11字符
	buffer_size = 11;
	NAPI_CALL(env,
		napi_get_value_string_utf8(env, args[1], req.BrokerID, buffer_size, &copied));

	CTPTraderClient* traderClient;
	NAPI_CALL(env, napi_unwrap(env, _this, reinterpret_cast<void**>(&traderClient)));

	//Js主线程请求号递增
	requestID++;
	//c++ void*泛型指针
	int nResult = traderClient->invoke(&req, T_QuerySettlementInfo_RE, requestID);

	napi_value result;
	NAPI_CALL(env, napi_create_int32(env, nResult, &result));

	return result;
}

napi_value CTPTraderClient::confirmSettlementInfo(napi_env env, napi_callback_info info)
{
	size_t argc = 2;
	napi_value args[2];
	napi_value _this;
	NAPI_CALL(env, napi_get_cb_info(env, info, &argc, args, &_this, nullptr));

	if (argc != 2) {
		napi_value msg;
		napi_create_string_utf8(env,
			"Wrong number of arguments.Right Format:confirmSettlementInfo(String:userID, String::brokerID)",
			NAPI_AUTO_LENGTH, &msg);
		napi_value err;
		napi_create_error(env, NULL, msg, &err);
		napi_fatal_exception(env, err);
		return NULL;
	}

	napi_valuetype investorIDType;
	NAPI_CALL(env, napi_typeof(env, args[0], &investorIDType));

	napi_valuetype brokerIDType;
	NAPI_CALL(env, napi_typeof(env, args[1], &brokerIDType));

	if (investorIDType != napi_string
		|| brokerIDType != napi_string) {
		napi_value msg;
		napi_create_string_utf8(env,
			"Parameter Type Error,Right Format:confirmSettlementInfo(String:userID, String::brokerID)",
			NAPI_AUTO_LENGTH, &msg);
		napi_value err;
		napi_create_error(env, NULL, msg, &err);
		napi_fatal_exception(env, err);
		return NULL;
	}


	CThostFtdcSettlementInfoConfirmField req;
	memset(&req, 0, sizeof(req));

	//InvestorID 13字符
	size_t buffer_size = 13;
	size_t copied;

	NAPI_CALL(env,
		napi_get_value_string_utf8(env, args[0], req.InvestorID, buffer_size, &copied));

	//BrokerID 11字符
	buffer_size = 11;
	NAPI_CALL(env,
		napi_get_value_string_utf8(env, args[1], req.BrokerID, buffer_size, &copied));

	CTPTraderClient* traderClient;
	NAPI_CALL(env, napi_unwrap(env, _this, reinterpret_cast<void**>(&traderClient)));

	//Js主线程请求号递增
	requestID++;
	//c++ void*泛型指针
	int nResult = traderClient->invoke(&req, T_CONFIRMSETTLEMENT_RE, requestID);

	napi_value result;
	NAPI_CALL(env, napi_create_int32(env, nResult, &result));

	return result;
}

napi_value CTPTraderClient::queryInstrument(napi_env env, napi_callback_info info)
{
	size_t argc = 0;
	napi_value _this;
	NAPI_CALL(env, napi_get_cb_info(env, info, &argc, nullptr, &_this, nullptr));

	if (argc != 0) {
		napi_value msg;
		napi_create_string_utf8(env,
			"Wrong number of arguments.Right Format: queryInstrument()",
			NAPI_AUTO_LENGTH, &msg);
		napi_value err;
		napi_create_error(env, NULL, msg, &err);
		napi_fatal_exception(env, err);
		return NULL;
	}

	CThostFtdcQryInstrumentField req;
	memset(&req, 0, sizeof(req));

	CTPTraderClient* traderClient;
	NAPI_CALL(env, napi_unwrap(env, _this, reinterpret_cast<void**>(&traderClient)));

	//Js主线程请求号递增
	requestID++;
	//c++ void*泛型指针
	int nResult = traderClient->invoke(&req, T_QRYINSTRUMENT_RE, requestID);

	napi_value result;
	NAPI_CALL(env, napi_create_int32(env, nResult, &result));

	return result;
}

napi_value CTPTraderClient::queryTradingAccount(napi_env env, napi_callback_info info)
{
	size_t argc = 2;
	napi_value args[2];
	napi_value _this;
	NAPI_CALL(env, napi_get_cb_info(env, info, &argc, args, &_this, nullptr));

	if (argc != 2) {
		napi_value msg;
		napi_create_string_utf8(env,
			"Wrong number of arguments.Right Format:queryTradingAccount(String:UserID, String:BrokerID)",
			NAPI_AUTO_LENGTH, &msg);
		napi_value err;
		napi_create_error(env, NULL, msg, &err);
		napi_fatal_exception(env, err);
		return NULL;
	}

	napi_valuetype investorIDType;
	NAPI_CALL(env, napi_typeof(env, args[0], &investorIDType));

	napi_valuetype brokerIDType;
	NAPI_CALL(env, napi_typeof(env, args[1], &brokerIDType));

	if (investorIDType != napi_string
		|| brokerIDType != napi_string) {
		napi_value msg;
		napi_create_string_utf8(env,
			"Parameter Type Error,Right Format:queryTradingAccount(String:UserID, String:BrokerID)",
			NAPI_AUTO_LENGTH, &msg);
		napi_value err;
		napi_create_error(env, NULL, msg, &err);
		napi_fatal_exception(env, err);
		return NULL;
	}

	CThostFtdcQryTradingAccountField req;
	memset(&req, 0, sizeof(req));

	//InvestorID 13字符
	size_t buffer_size = 13;
	size_t copied;

	NAPI_CALL(env,
		napi_get_value_string_utf8(env, args[0], req.InvestorID, buffer_size, &copied));

	//BrokerID 11字符
	buffer_size = 11;
	NAPI_CALL(env,
		napi_get_value_string_utf8(env, args[1], req.BrokerID, buffer_size, &copied));

	CTPTraderClient* traderClient;
	NAPI_CALL(env, napi_unwrap(env, _this, reinterpret_cast<void**>(&traderClient)));

	//Js主线程请求号递增
	requestID++;
	//c++ void*泛型指针
	int nResult = traderClient->invoke(&req, T_QRYTRADINGACCOUNT_RE, requestID);
	if (nResult == 0)
	{
		nResult = requestID;
	}

	napi_value result;
	NAPI_CALL(env, napi_create_int32(env, nResult, &result));

	return result;
}

napi_value CTPTraderClient::queryInvestorPosition(napi_env env, napi_callback_info info)
{
	size_t argc = 2;
	napi_value args[2];
	napi_value _this;
	NAPI_CALL(env, napi_get_cb_info(env, info, &argc, args, &_this, nullptr));

	if (argc != 2) {
		napi_value msg;
		napi_create_string_utf8(env,
			"Wrong number of arguments.Right Format:queryInvestorPosition(String:UserID, String:BrokerID)",
			NAPI_AUTO_LENGTH, &msg);
		napi_value err;
		napi_create_error(env, NULL, msg, &err);
		napi_fatal_exception(env, err);
		return NULL;
	}

	napi_valuetype investorIDType;
	NAPI_CALL(env, napi_typeof(env, args[0], &investorIDType));

	napi_valuetype brokerIDType;
	NAPI_CALL(env, napi_typeof(env, args[1], &brokerIDType));

	if (investorIDType != napi_string
		|| brokerIDType != napi_string) {
		napi_value msg;
		napi_create_string_utf8(env,
			"Parameter Type Error,Right Format:queryInvestorPosition(String:UserID, String:BrokerID)",
			NAPI_AUTO_LENGTH, &msg);
		napi_value err;
		napi_create_error(env, NULL, msg, &err);
		napi_fatal_exception(env, err);
		return NULL;
	}

	CThostFtdcQryInvestorPositionField req;
	memset(&req, 0, sizeof(req));

	//InvestorID 13字符
	size_t buffer_size = 13;
	size_t copied;

	NAPI_CALL(env,
		napi_get_value_string_utf8(env, args[0], req.InvestorID, buffer_size, &copied));

	//BrokerID 11字符
	buffer_size = 11;
	NAPI_CALL(env,
		napi_get_value_string_utf8(env, args[1], req.BrokerID, buffer_size, &copied));

	CTPTraderClient* traderClient;
	NAPI_CALL(env, napi_unwrap(env, _this, reinterpret_cast<void**>(&traderClient)));

	//Js主线程请求号递增
	requestID++;
	//c++ void*泛型指针
	int nResult = traderClient->invoke(&req, T_QRYINVESTORPOSITION_RE, requestID);

	napi_value result;
	NAPI_CALL(env, napi_create_int32(env, nResult, &result));

	return result;
}

napi_value CTPTraderClient::queryCommissionRate(napi_env env, napi_callback_info info)
{
	size_t argc = 3;
	napi_value args[3];
	napi_value _this;
	NAPI_CALL(env, napi_get_cb_info(env, info, &argc, args, &_this, nullptr));

	if (argc != 3) {
		napi_value msg;
		napi_create_string_utf8(env,
			"Wrong number of arguments.Right Format:queryCommissionRate(String:UserID, String:BrokerID,String:InstrumentID)",
			NAPI_AUTO_LENGTH, &msg);
		napi_value err;
		napi_create_error(env, NULL, msg, &err);
		napi_fatal_exception(env, err);
		return NULL;
	}

	napi_valuetype investorIDType;
	NAPI_CALL(env, napi_typeof(env, args[0], &investorIDType));

	napi_valuetype brokerIDType;
	NAPI_CALL(env, napi_typeof(env, args[1], &brokerIDType));

	napi_valuetype instrumentIDType;
	NAPI_CALL(env, napi_typeof(env, args[2], &instrumentIDType));

	if (investorIDType != napi_string
		|| brokerIDType != napi_string
		|| instrumentIDType != napi_string) {
		napi_value msg;
		napi_create_string_utf8(env,
			"Wrong number of arguments.Right Format:queryCommissionRate(String:UserID, String:BrokerID,String:InstrumentID)",
			NAPI_AUTO_LENGTH, &msg);
		napi_value err;
		napi_create_error(env, NULL, msg, &err);
		napi_fatal_exception(env, err);
		return NULL;
	}

	CThostFtdcQryInstrumentCommissionRateField  req;
	memset(&req, 0, sizeof(req));

	//InvestorID 13字符
	size_t buffer_size = 13;
	size_t copied;

	NAPI_CALL(env,
		napi_get_value_string_utf8(env, args[0], req.InvestorID, buffer_size, &copied));

	//BrokerID 11字符
	buffer_size = 11;
	NAPI_CALL(env,
		napi_get_value_string_utf8(env, args[1], req.BrokerID, buffer_size, &copied));
	
	//InstrumentID 31字符
	buffer_size = 31;
	NAPI_CALL(env,
		napi_get_value_string_utf8(env, args[2], req.InstrumentID, buffer_size, &copied));

	CTPTraderClient* traderClient;
	NAPI_CALL(env, napi_unwrap(env, _this, reinterpret_cast<void**>(&traderClient)));


	//Js主线程请求号递增
	requestID++;
	//c++ void*泛型指针
	int nResult = traderClient->invoke(&req, T_QueryCommissionRate_RE, requestID);

	napi_value result;
	NAPI_CALL(env, napi_create_int32(env, nResult, &result));

	return result;
}

napi_value CTPTraderClient::sendOrder(napi_env env, napi_callback_info info)
{
	size_t argc = 1;
	napi_value args[1];
	napi_value _this;
	NAPI_CALL(env, napi_get_cb_info(env, info, &argc, args, &_this, nullptr));

	if (argc != 1) {
		napi_value msg;
		napi_create_string_utf8(env,
			"Wrong number of arguments.Right Format: sendOrder(Object:requestObj)",
			NAPI_AUTO_LENGTH, &msg);
		napi_value err;
		napi_create_error(env, NULL, msg, &err);
		napi_fatal_exception(env, err);
		return NULL;
	}

	napi_valuetype sendOrderInfoType;
	NAPI_CALL(env, napi_typeof(env, args[0], &sendOrderInfoType));

	if (sendOrderInfoType != napi_object) {
		napi_value msg;
		napi_create_string_utf8(env,
			"Parameter Type Error,Right Format: sendOrder(Object:requestObj)",
			NAPI_AUTO_LENGTH, &msg);
		napi_value err;
		napi_create_error(env, NULL, msg, &err);
		napi_fatal_exception(env, err);
		return NULL;
	}

	napi_value OrderInfo = args[0];

	size_t buffer_size = 100;
	size_t copied;


	///1.经纪公司代码
	//TThostFtdcBrokerIDType	BrokerID;
	//typedef char TThostFtdcBrokerIDType[11];
	napi_value _BrokerID;
	NAPI_CALL(env, napi_create_string_utf8(env, "BrokerID",NAPI_AUTO_LENGTH, &_BrokerID));

	napi_value brokerID;
	NAPI_CALL(env, napi_get_property(env, OrderInfo, _BrokerID, &brokerID));

	//BrokerID 11字符
	char BrokerID[11];
	buffer_size = 11;
	NAPI_CALL(env,
		napi_get_value_string_utf8(env, brokerID, BrokerID, buffer_size, &copied));


	///2.投资者代码
	//TThostFtdcInvestorIDType	InvestorID;
	//typedef char TThostFtdcInvestorIDType[13];
	napi_value _InvestorID;
	NAPI_CALL(env, napi_create_string_utf8(env, "InvestorID", NAPI_AUTO_LENGTH, &_InvestorID));

	napi_value investorID;
	NAPI_CALL(env, napi_get_property(env, OrderInfo, _InvestorID, &investorID));

	//InvestorID 13字符
	char InvestorID[13];
	buffer_size = 13;
	NAPI_CALL(env,
		napi_get_value_string_utf8(env, investorID, InvestorID, buffer_size, &copied));


	///3.合约代码
	//TThostFtdcInstrumentIDType	InstrumentID;
	//typedef char TThostFtdcInstrumentIDType[31];
	napi_value _InstrumentID;
	NAPI_CALL(env, napi_create_string_utf8(env, "InstrumentID", NAPI_AUTO_LENGTH, &_InstrumentID));
	
	napi_value instrumentID;
	NAPI_CALL(env, napi_get_property(env, OrderInfo, _InstrumentID, &instrumentID));
	
	//InstrumentID 31字符
	char InstrumentID[31];
	buffer_size = 31;
	NAPI_CALL(env,
		napi_get_value_string_utf8(env, instrumentID, InstrumentID, buffer_size, &copied));


	///4.报单引用
	//TThostFtdcOrderRefType	OrderRef;
	//typedef char TThostFtdcOrderRefType[13];
	napi_value _OrderRef;
	NAPI_CALL(env, napi_create_string_utf8(env, "OrderRef", NAPI_AUTO_LENGTH, &_OrderRef));

	napi_value orderRef;
	NAPI_CALL(env, napi_get_property(env, OrderInfo, _OrderRef, &orderRef));

	//OrderRef 13字符
	char OrderRef[13];
	buffer_size = 13;
	NAPI_CALL(env,
		napi_get_value_string_utf8(env, orderRef, OrderRef, buffer_size, &copied));


	///5.用户代码
	//TThostFtdcUserIDType	UserID;
	//typedef char TThostFtdcUserIDType[16];
	napi_value _UserID;
	NAPI_CALL(env, napi_create_string_utf8(env, "UserID", NAPI_AUTO_LENGTH, &_UserID));

	napi_value userID;
	NAPI_CALL(env, napi_get_property(env, OrderInfo, _UserID, &userID));
	
	char UserID[16];
	buffer_size = 16;
	NAPI_CALL(env,
		napi_get_value_string_utf8(env, userID, UserID, buffer_size, &copied));

	///6.报单价格条件
	//TThostFtdcOrderPriceTypeType	OrderPriceType;
	//typedef char TThostFtdcOrderPriceTypeType;
	napi_value _OrderPriceType;
	NAPI_CALL(env, napi_create_string_utf8(env, "OrderPriceType", NAPI_AUTO_LENGTH, &_OrderPriceType));
	
	napi_value orderPriceType;
	NAPI_CALL(env, napi_get_property(env, OrderInfo, _OrderPriceType, &orderPriceType));

	char OrderPriceType[2];
	buffer_size = 2;
	//napi_get_value_string_utf8最少2个字符！因为JS层1个字符其实是2个字符
	NAPI_CALL(env,
		napi_get_value_string_utf8(env, orderPriceType, OrderPriceType, buffer_size, &copied));

	///7.买卖方向
	//TThostFtdcDirectionType	Direction;
	//typedef char TThostFtdcDirectionType;
	napi_value _Direction;
	NAPI_CALL(env, napi_create_string_utf8(env, "Direction", NAPI_AUTO_LENGTH, &_Direction));

	napi_value direction;
	NAPI_CALL(env, napi_get_property(env, OrderInfo, _Direction, &direction));
	
	char Direction[2];
	buffer_size = 2;
	NAPI_CALL(env,
		napi_get_value_string_utf8(env, direction, Direction, buffer_size, &copied));

	///8.组合开平标志
	//TThostFtdcCombOffsetFlagType	CombOffsetFlag;
	//typedef char TThostFtdcCombOffsetFlagType[5];
	napi_value _CombOffsetFlag;
	NAPI_CALL(env, napi_create_string_utf8(env, "CombOffsetFlag", NAPI_AUTO_LENGTH, &_CombOffsetFlag));
	
	napi_value combOffsetFlag;
	NAPI_CALL(env, napi_get_property(env, OrderInfo, _CombOffsetFlag, &combOffsetFlag));
	
	char CombOffsetFlag[5];
	buffer_size = 5;
	NAPI_CALL(env,
		napi_get_value_string_utf8(env, combOffsetFlag, CombOffsetFlag, buffer_size, &copied));


	///9.组合投机套保标志
	//TThostFtdcCombHedgeFlagType	CombHedgeFlag;
	//typedef char TThostFtdcCombHedgeFlagType[5];
	napi_value _CombHedgeFlag;
	NAPI_CALL(env, napi_create_string_utf8(env, "CombHedgeFlag", NAPI_AUTO_LENGTH, &_CombHedgeFlag));
	
	napi_value combHedgeFlag;
	NAPI_CALL(env, napi_get_property(env, OrderInfo, _CombHedgeFlag, &combHedgeFlag));

	char CombHedgeFlag[5];
	buffer_size = 5;
	NAPI_CALL(env,
		napi_get_value_string_utf8(env, combHedgeFlag, CombHedgeFlag, buffer_size, &copied));


	///10.价格
	//TThostFtdcPriceType	LimitPrice;
	//typedef double TThostFtdcPriceType;
	napi_value _LimitPrice;
	NAPI_CALL(env, napi_create_string_utf8(env, "LimitPrice", NAPI_AUTO_LENGTH, &_LimitPrice));
	
	napi_value limitPrice;
	NAPI_CALL(env, napi_get_property(env, OrderInfo, _LimitPrice, &limitPrice));

	double LimitPrice;
	NAPI_CALL(env,napi_get_value_double(env, limitPrice, &LimitPrice));


	///11.数量
	//TThostFtdcVolumeType	VolumeTotalOriginal;
	//typedef int TThostFtdcVolumeType;
	napi_value _VolumeTotalOriginal;
	NAPI_CALL(env, napi_create_string_utf8(env, "VolumeTotalOriginal", NAPI_AUTO_LENGTH, &_VolumeTotalOriginal));

	napi_value volumeTotalOriginal;
	NAPI_CALL(env, napi_get_property(env, OrderInfo, _VolumeTotalOriginal, &volumeTotalOriginal));

	int VolumeTotalOriginal;
	NAPI_CALL(env, napi_get_value_int32(env, volumeTotalOriginal, &VolumeTotalOriginal));

	///12.有效期类型
	//TThostFtdcTimeConditionType	TimeCondition;
	//typedef char TThostFtdcTimeConditionType;
	napi_value _TimeCondition;
	NAPI_CALL(env, napi_create_string_utf8(env, "TimeCondition", NAPI_AUTO_LENGTH, &_TimeCondition));

	napi_value timeCondition;
	NAPI_CALL(env, napi_get_property(env, OrderInfo, _TimeCondition, &timeCondition));

	char TimeCondition[2];
	buffer_size = 2;
	NAPI_CALL(env,
		napi_get_value_string_utf8(env, timeCondition, TimeCondition, buffer_size, &copied));

	///13.成交量类型
	//TThostFtdcVolumeConditionType	VolumeCondition;
	//typedef char TThostFtdcVolumeConditionType;
	napi_value _VolumeCondition;
	NAPI_CALL(env, napi_create_string_utf8(env, "VolumeCondition", NAPI_AUTO_LENGTH, &_VolumeCondition));

	napi_value volumeCondition;
	NAPI_CALL(env, napi_get_property(env, OrderInfo, _VolumeCondition, &volumeCondition));
	
	char VolumeCondition[2];
	buffer_size = 2;
	NAPI_CALL(env,
		napi_get_value_string_utf8(env, volumeCondition, VolumeCondition, buffer_size, &copied));


	///14.最小成交量
	//TThostFtdcVolumeType	MinVolume;
	//typedef int TThostFtdcVolumeType;
	napi_value _MinVolume;
	NAPI_CALL(env, napi_create_string_utf8(env, "MinVolume", NAPI_AUTO_LENGTH, &_MinVolume));
	
	napi_value minVolume;
	NAPI_CALL(env, napi_get_property(env, OrderInfo, _MinVolume, &minVolume));

	int MinVolume;
	NAPI_CALL(env, napi_get_value_int32(env, minVolume, &MinVolume));

	///15.触发条件
	//TThostFtdcContingentConditionType	ContingentCondition;
	//typedef char TThostFtdcContingentConditionType;
	napi_value _ContingentCondition;
	NAPI_CALL(env, napi_create_string_utf8(env, "ContingentCondition", NAPI_AUTO_LENGTH, &_ContingentCondition));

	napi_value contingentCondition;
	NAPI_CALL(env, napi_get_property(env, OrderInfo, _ContingentCondition, &contingentCondition));

	char ContingentCondition[2];
	buffer_size = 2;
	NAPI_CALL(env,
		napi_get_value_string_utf8(env, contingentCondition, ContingentCondition, buffer_size, &copied));


	///16.止损价
	//TThostFtdcPriceType	StopPrice;
	//typedef double TThostFtdcPriceType;
	napi_value _StopPrice;
	NAPI_CALL(env, napi_create_string_utf8(env, "StopPrice", NAPI_AUTO_LENGTH, &_StopPrice));

	napi_value stopPrice;
	NAPI_CALL(env, napi_get_property(env, OrderInfo, _StopPrice, &stopPrice));

	double StopPrice;
	NAPI_CALL(env, napi_get_value_double(env, stopPrice, &StopPrice));


	///17.强平原因
	//TThostFtdcForceCloseReasonType	ForceCloseReason;
	//typedef char TThostFtdcForceCloseReasonType;
	napi_value _ForceCloseReason;
	NAPI_CALL(env, napi_create_string_utf8(env, "ForceCloseReason", NAPI_AUTO_LENGTH, &_ForceCloseReason));

	napi_value forceCloseReason;
	NAPI_CALL(env, napi_get_property(env, OrderInfo, _ForceCloseReason, &forceCloseReason));

	char ForceCloseReason[2];
	buffer_size = 2;
	NAPI_CALL(env,
		napi_get_value_string_utf8(env, forceCloseReason, ForceCloseReason, buffer_size, &copied));


	///18.自动挂起标志
	//TThostFtdcBoolType	IsAutoSuspend;
	//typedef int TThostFtdcBoolType;
	napi_value _IsAutoSuspend;
	NAPI_CALL(env, napi_create_string_utf8(env, "IsAutoSuspend", NAPI_AUTO_LENGTH, &_IsAutoSuspend));

	napi_value isAutoSuspend;
	NAPI_CALL(env, napi_get_property(env, OrderInfo, _IsAutoSuspend, &isAutoSuspend));

	int IsAutoSuspend;
	NAPI_CALL(env, napi_get_value_int32(env, isAutoSuspend, &IsAutoSuspend));


	///19.用户强评标志
	//TThostFtdcBoolType	UserForceClose;
	//typedef int TThostFtdcBoolType;
	napi_value _UserForceClose;
	NAPI_CALL(env, napi_create_string_utf8(env, "UserForceClose", NAPI_AUTO_LENGTH, &_UserForceClose));

	napi_value userForceClose;
	NAPI_CALL(env, napi_get_property(env, OrderInfo, _UserForceClose, &userForceClose));

	int UserForceClose;
	NAPI_CALL(env, napi_get_value_int32(env, userForceClose, &UserForceClose));



	CThostFtdcInputOrderField req;
	memset(&req, 0, sizeof(req));
	memcpy(req.BrokerID, BrokerID, strlen(BrokerID)+1);
	memcpy(req.InvestorID, InvestorID, strlen(InvestorID) + 1);
	memcpy(req.InstrumentID, InstrumentID, strlen(InstrumentID) + 1);
	memcpy(req.OrderRef, OrderRef, strlen(OrderRef) + 1);
	memcpy(req.UserID, UserID, strlen(UserID) + 1);
	memcpy(req.CombOffsetFlag, CombOffsetFlag, strlen(CombOffsetFlag) + 1);
	memcpy(req.CombHedgeFlag, CombHedgeFlag, strlen(CombHedgeFlag) + 1);

	req.OrderPriceType = OrderPriceType[0];
	req.Direction = Direction[0];
	req.LimitPrice = LimitPrice;
	req.VolumeTotalOriginal = VolumeTotalOriginal;
	
	req.TimeCondition = TimeCondition[0];
	
	req.VolumeCondition = VolumeCondition[0];

	req.MinVolume = MinVolume;

	req.ContingentCondition = ContingentCondition[0];

	req.StopPrice = StopPrice;

	req.ForceCloseReason = ForceCloseReason[0];

	req.IsAutoSuspend = IsAutoSuspend;
	req.UserForceClose = UserForceClose;



	CTPTraderClient* traderClient;
	NAPI_CALL(env, napi_unwrap(env, _this, reinterpret_cast<void**>(&traderClient)));

	//Js主线程请求号递增
	requestID++;
	//c++ void*泛型指针
	int nResult = traderClient->invoke(&req, T_INSERTORDER_RE, requestID);

	napi_value result;
	NAPI_CALL(env, napi_create_int32(env, nResult, &result));

	return result;
}

napi_value CTPTraderClient::cancelOrder(napi_env env, napi_callback_info info)
{
	size_t argc = 1;
	napi_value args[1];
	napi_value _this;
	NAPI_CALL(env, napi_get_cb_info(env, info, &argc, args, &_this, nullptr));

	if (argc != 1) {
		napi_value msg;
		napi_create_string_utf8(env,
			"Wrong number of arguments.Right Format:cancelOrder(Object:req)",
			NAPI_AUTO_LENGTH, &msg);
		napi_value err;
		napi_create_error(env, NULL, msg, &err);
		napi_fatal_exception(env, err);
		return NULL;
	}

	napi_valuetype orderInfoType;
	NAPI_CALL(env, napi_typeof(env, args[0], &orderInfoType));

	if (orderInfoType != napi_object) {
		napi_value msg;
		napi_create_string_utf8(env,
			"Parameter Type Error,Right Format: cancelOrder(Object:req)",
			NAPI_AUTO_LENGTH, &msg);
		napi_value err;
		napi_create_error(env, NULL, msg, &err);
		napi_fatal_exception(env, err);
		return NULL;
	}

	napi_value OrderInfo = args[0];


	CThostFtdcInputOrderActionField req;
	memset(&req, 0, sizeof(req));
	
	size_t buffer_size = 100;
	size_t copied;


	///1.合约代码
	//TThostFtdcInstrumentIDType	InstrumentID;
	//typedef char TThostFtdcInstrumentIDType[31];
	napi_value InstrumentID;
	NAPI_CALL(env, napi_create_string_utf8(env, "InstrumentID", NAPI_AUTO_LENGTH, &InstrumentID));

	napi_value instrumentID;
	NAPI_CALL(env, napi_get_property(env, OrderInfo, InstrumentID, &instrumentID));

	//InstrumentID 31字符
	buffer_size = 31;
	NAPI_CALL(env,
		napi_get_value_string_utf8(env, instrumentID, req.InstrumentID, buffer_size, &copied));

	///2.交易所ID
	//ThostFtdcExchangeIDType	ExchangeID;
	//typedef char TThostFtdcExchangeIDType[9];
	napi_value ExchangeID;
	NAPI_CALL(env, napi_create_string_utf8(env, "ExchangeID", NAPI_AUTO_LENGTH, &ExchangeID));

	napi_value exchangeID;
	NAPI_CALL(env, napi_get_property(env, OrderInfo, ExchangeID, &exchangeID));

	//ExchangeID 9字符
	buffer_size = 9;
	NAPI_CALL(env,
		napi_get_value_string_utf8(env, exchangeID, req.ExchangeID, buffer_size, &copied));

	///3.报单引用
	//TThostFtdcOrderRefType	OrderRef;
	//typedef char TThostFtdcOrderRefType[13];
	napi_value OrderRef;
	NAPI_CALL(env, napi_create_string_utf8(env, "OrderRef", NAPI_AUTO_LENGTH, &OrderRef));

	napi_value orderRef;
	NAPI_CALL(env, napi_get_property(env, OrderInfo, OrderRef, &orderRef));

	//OrderRef 13字符
	buffer_size = 13;
	NAPI_CALL(env,
		napi_get_value_string_utf8(env, orderRef, req.OrderRef, buffer_size, &copied));

	///4.前置编号
	//TThostFtdcFrontIDType	FrontID;
	//typedef int TThostFtdcFrontIDType;
	napi_value FrontID;
	NAPI_CALL(env, napi_create_string_utf8(env, "FrontID", NAPI_AUTO_LENGTH, &FrontID));

	napi_value frontID;
	NAPI_CALL(env, napi_get_property(env, OrderInfo, FrontID, &frontID));

	NAPI_CALL(env, napi_get_value_int32(env, frontID, &req.FrontID));

	///5.会话编号
	//TThostFtdcSessionIDType	SessionID;
	//typedef int TThostFtdcSessionIDType;
	napi_value SessionID;
	NAPI_CALL(env, napi_create_string_utf8(env, "SessionID", NAPI_AUTO_LENGTH, &SessionID));

	napi_value sessionID;
	NAPI_CALL(env, napi_get_property(env, OrderInfo, SessionID, &sessionID));

	NAPI_CALL(env, napi_get_value_int32(env, sessionID, &req.SessionID));

	///6.经纪公司代码
	//TThostFtdcBrokerIDType	BrokerID;
	//typedef char TThostFtdcBrokerIDType[11];
	napi_value BrokerID;
	NAPI_CALL(env, napi_create_string_utf8(env, "BrokerID", NAPI_AUTO_LENGTH, &BrokerID));

	napi_value brokerID;
	NAPI_CALL(env, napi_get_property(env, OrderInfo, BrokerID, &brokerID));

	//BrokerID 11字符
	buffer_size = 11;
	NAPI_CALL(env,
		napi_get_value_string_utf8(env, brokerID, req.BrokerID, buffer_size, &copied));

	///7.投资者代码
	//TThostFtdcInvestorIDType	InvestorID;
	//typedef char TThostFtdcInvestorIDType[13];
	napi_value InvestorID;
	NAPI_CALL(env, napi_create_string_utf8(env, "InvestorID", NAPI_AUTO_LENGTH, &InvestorID));

	napi_value investorID;
	NAPI_CALL(env, napi_get_property(env, OrderInfo, InvestorID, &investorID));

	//InvestorID 13字符
	buffer_size = 13;
	NAPI_CALL(env,
		napi_get_value_string_utf8(env, investorID, req.InvestorID, buffer_size, &copied));

	///8.报单编号
	//TThostFtdcOrderSysIDType	OrderSysID;
	//typedef char TThostFtdcOrderSysIDType[21];
	napi_value OrderSysID;
	NAPI_CALL(env, napi_create_string_utf8(env, "OrderSysID", NAPI_AUTO_LENGTH, &OrderSysID));

	napi_value orderSysID;
	NAPI_CALL(env, napi_get_property(env, OrderInfo, OrderSysID, &orderSysID));

	//OrderSysID 21字符
	buffer_size = 21;
	NAPI_CALL(env,
		napi_get_value_string_utf8(env, orderSysID, req.OrderSysID, buffer_size, &copied));

	//9.操作标志
	//TThostFtdcActionFlagType	ActionFlag;
	req.ActionFlag = THOST_FTDC_AF_Delete;

	//FrontID + SessionID + OrderRef
	//这组报单序列号可以由客户端自行维护，客户端可以通过该序列号随时进行撤单操作。
	CTPTraderClient* traderClient;
	NAPI_CALL(env, napi_unwrap(env, _this, reinterpret_cast<void**>(&traderClient)));

	//Js主线程请求号递增
	requestID++;
	//c++ void*泛型指针
	int nResult = traderClient->invoke(&req, T_INPUTORDERACTION_RE, requestID);

	napi_value result;
	NAPI_CALL(env, napi_create_int32(env, nResult, &result));

	return result;
}

napi_value CTPTraderClient::logout(napi_env env, napi_callback_info info)
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

	if (userIDType != napi_string ||
		brokerIDType != napi_string) {
		napi_value msg;
		napi_create_string_utf8(env,
			"Parameter Type Error,Right Format:logout(String:userID, String::brokerID)",
			NAPI_AUTO_LENGTH, &msg);
		napi_value err;
		napi_create_error(env, NULL, msg, &err);
		napi_fatal_exception(env, err);
		return NULL;
	}

	CThostFtdcUserLogoutField req;
	memset(&req, 0, sizeof(req));

	//UserID 16字符
	//typedef char TThostFtdcUserIDType[16];
	size_t buffer_size = 16;
	size_t copied;

	NAPI_CALL(env,
		napi_get_value_string_utf8(env, args[0], req.UserID, buffer_size, &copied));
	
	//BrokerID 11字符
	//typedef char TThostFtdcBrokerIDType[11];
	buffer_size = 11;
	NAPI_CALL(env,
		napi_get_value_string_utf8(env, args[1], req.BrokerID, buffer_size, &copied));


	CTPTraderClient* traderClient;
	NAPI_CALL(env, napi_unwrap(env, _this, reinterpret_cast<void**>(&traderClient)));


	//Js主线程请求号递增
	requestID++;
	//c++ void*泛型指针
	int nResult = traderClient->invoke(&req, T_LOGOUT_RE, requestID);

	napi_value result;
	NAPI_CALL(env, napi_create_int32(env, nResult, &result));

	return result;
}

int CTPTraderClient::invoke(void* field, int fuctionType, int requestID)
{
	int nResult = 0;

	switch (fuctionType)
	{
		case T_CONNECT_RE:
		{
			//订阅模式
			//公有流：交易所向所有连接着的客户端发布的信息。比如说：合约在场上交易状态。可交易，或者不可交易
			//私有流：交易所向特定客户端发送的信息。如报单回报，成交回报。
			/*
			enum THOST_TE_RESUME_TYPE
			{
			THOST_TERT_RESTART = 0,接收所有交易所当日曾发送过的以及之后可能会发送的所有该类消息。
			THOST_TERT_RESUME,     接收客户端上次断开连接后交易所曾发送过的以及之后可能会发送的所有该类消息。
			THOST_TERT_QUICK       接收客户端登录之后交易所可能会发送的所有该类消息。
			};*/

			ConnectField* _pConnectF = static_cast<ConnectField*>(field);
			this->Api = CThostFtdcTraderApi::CreateFtdcTraderApi(_pConnectF->flowPath);
			this->Api->RegisterSpi(this);
			this->Api->RegisterFront(_pConnectF->front_addr);
			this->Api->SubscribePrivateTopic(THOST_TERT_QUICK);
			this->Api->SubscribePublicTopic(THOST_TERT_QUICK);
			this->Api->Init();
			break;
		}case T_Authenticate_RE:
		{
			CThostFtdcReqAuthenticateField *_pReqAuthenticateField = static_cast<CThostFtdcReqAuthenticateField*>(field);
			nResult = this->Api->ReqAuthenticate(_pReqAuthenticateField, requestID);

			break;
		}case T_LOGIN_RE:
		{
			CThostFtdcReqUserLoginField *_pReqUserLoginField = static_cast<CThostFtdcReqUserLoginField*>(field);
			nResult = this->Api->ReqUserLogin(_pReqUserLoginField, requestID);

			break;
		}case T_QuerySettlementInfo_RE:
		{
			CThostFtdcQrySettlementInfoField *pQrySettlementInfoField = static_cast<CThostFtdcQrySettlementInfoField*>(field);
			nResult = this->Api->ReqQrySettlementInfo(pQrySettlementInfoField, requestID);

			break;
		}case T_CONFIRMSETTLEMENT_RE:
		{
			CThostFtdcSettlementInfoConfirmField *pSettlementInfoConfirmField = static_cast<CThostFtdcSettlementInfoConfirmField*>(field);
			nResult = this->Api->ReqSettlementInfoConfirm(pSettlementInfoConfirmField, requestID);

			break;
		}case T_QRYINSTRUMENT_RE:
		{
			CThostFtdcQryInstrumentField *pQryInstrumentField = static_cast<CThostFtdcQryInstrumentField*>(field);
			nResult = this->Api->ReqQryInstrument(pQryInstrumentField, requestID);

			break;
		}case T_QRYTRADINGACCOUNT_RE:
		{
			CThostFtdcQryTradingAccountField *pQryTradingAccountField = static_cast<CThostFtdcQryTradingAccountField*>(field);
			nResult = this->Api->ReqQryTradingAccount(pQryTradingAccountField, requestID);

			break;
		}case T_QRYINVESTORPOSITION_RE:
		{
			CThostFtdcQryInvestorPositionField *pQryInvestorPositionField = static_cast<CThostFtdcQryInvestorPositionField*>(field);
			nResult = this->Api->ReqQryInvestorPosition(pQryInvestorPositionField, requestID);

			break;
		}case T_INSERTORDER_RE:
		{
			CThostFtdcInputOrderField *pInputOrderField = static_cast<CThostFtdcInputOrderField*>(field);
			nResult = this->Api->ReqOrderInsert(pInputOrderField, requestID);
			//如果发送成功返回交易OrderID
			if (nResult == 0)
			{
				int OrderRef;
				sscanf(pInputOrderField->OrderRef, "%d", &OrderRef);
				nResult = OrderRef;
			}

			break;
		}case T_INPUTORDERACTION_RE:
		{
			CThostFtdcInputOrderActionField *pInputOrderActionField = static_cast<CThostFtdcInputOrderActionField*>(field);
			nResult = this->Api->ReqOrderAction(pInputOrderActionField, requestID);

			break;
		}case T_LOGOUT_RE:
		{
			CThostFtdcUserLogoutField* pUserLogout = static_cast<CThostFtdcUserLogoutField*>(field);
			nResult = this->Api->ReqUserLogout(pUserLogout, requestID);
			break;
		}case T_QueryCommissionRate_RE:
		{
			CThostFtdcQryInstrumentCommissionRateField* pQryInstrumentCommissionRateField = static_cast<CThostFtdcQryInstrumentCommissionRateField*>(field);
			nResult = this->Api->ReqQryInstrumentCommissionRate(pQryInstrumentCommissionRateField, requestID);
			break;
		}default:
		{
			break;
		}
	};

	return nResult;
}

//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

void CTPTraderClient::OnFrontConnected()
{
	OnEventCbRtnField* field = new OnEventCbRtnField();//调用完毕后需要销毁
	field->eFlag = T_On_FrontConnected;                //FrontConnected

	queueEvent(field);                                 //对象销毁后，指针清空
}


void CTPTraderClient::OnFrontDisconnected(int nReason)
{
	OnEventCbRtnField* field = new OnEventCbRtnField();
	field->eFlag = T_On_FrontDisconnected;
	field->nReason = nReason;

	queueEvent(field);
}

void  CTPTraderClient::on_invoke(int event_type, void* _stru, CThostFtdcRspInfoField *pRspInfo_org, int nRequestID, bool bIsLast)
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
}


void  CTPTraderClient::process_event(OnEventCbRtnField* cbTrnField)
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
		}case T_On_RspAuthenticate:
		{
			napi_value argv[4];
			pkg_cb_rspauthenticate(cbTrnField, argv);

			NAPI_CALL_RETURN_VOID(env_, napi_call_function(env_, global, cb, 4, argv, NULL));

			break;
		}case T_On_RspUserLogin:
		{
			napi_value argv[4];
			pkg_cb_userlogin(cbTrnField, argv);

			NAPI_CALL_RETURN_VOID(env_, napi_call_function(env_, global, cb, 4, argv, NULL));

            break;
		}case T_On_RspQrySettlementInfo:
		{
			napi_value argv[4];
			pkg_cb_settlementInfo(cbTrnField, argv);

			NAPI_CALL_RETURN_VOID(env_, napi_call_function(env_, global, cb, 4, argv, NULL));

			break;
		}case T_On_RspSettlementInfoConfirm:
		{
			napi_value argv[4];
			pkg_cb_confirmsettlement(cbTrnField, argv);

			NAPI_CALL_RETURN_VOID(env_, napi_call_function(env_, global, cb, 4, argv, NULL));

			break;
		}case T_On_RspQryInstrument:
		{
			napi_value argv[4];
			pkg_cb_qryinstrument(cbTrnField, argv);

			NAPI_CALL_RETURN_VOID(env_, napi_call_function(env_, global, cb, 4, argv, NULL));
            
			break;
		}case T_On_RspQryTradingAccount:
		{
			napi_value argv[4];
			pkg_cb_qrytradingaccount(cbTrnField, argv);

			NAPI_CALL_RETURN_VOID(env_, napi_call_function(env_, global, cb, 4, argv, NULL));

			break;
		}case T_On_RspQryInvestorPosition:
		{
			napi_value argv[4];
			pkg_cb_qryinvestorposition(cbTrnField, argv);

			NAPI_CALL_RETURN_VOID(env_, napi_call_function(env_, global, cb, 4, argv, NULL));

			break;
		}case T_On_RspOrderInsert:
		{
			napi_value argv[4];
			pkg_cb_rspinsertorder(cbTrnField, argv);

			NAPI_CALL_RETURN_VOID(env_, napi_call_function(env_, global, cb, 4, argv, NULL));

			break;
		}case T_On_ErrRtnOrderInsert:
		{
			napi_value argv[2];
			pkg_cb_rspinsertorder(cbTrnField, argv);

			NAPI_CALL_RETURN_VOID(env_, napi_call_function(env_, global, cb, 2, argv, NULL));

			break;
		}case T_On_RtnOrder:
		{
			napi_value argv[1];
			pkg_cb_rtnorder(cbTrnField, argv);

			NAPI_CALL_RETURN_VOID(env_, napi_call_function(env_, global, cb, 1, argv, NULL));

			break;
		}case T_On_RtnTrade:
		{
			napi_value argv[1];
			pkg_cb_rtntrade(cbTrnField, argv);

			NAPI_CALL_RETURN_VOID(env_, napi_call_function(env_, global, cb, 1, argv, NULL));

			break;
		}case T_On_RspOrderAction:
		{
			napi_value argv[4];
			pkg_cb_rsporderaction(cbTrnField, argv);

			NAPI_CALL_RETURN_VOID(env_, napi_call_function(env_, global, cb, 4, argv, NULL));

			break;
		}case T_On_ErrRtnOrderAction:
		{
			napi_value argv[2];
			pkg_cb_errrtnorderaction(cbTrnField, argv);

			NAPI_CALL_RETURN_VOID(env_, napi_call_function(env_, global, cb, 2, argv, NULL));

			break;

		}case T_On_RspUserLogout:
		{
			napi_value argv[4];
			pkg_cb_rspuserlogout(cbTrnField, argv);

			NAPI_CALL_RETURN_VOID(env_, napi_call_function(env_, global, cb, 4, argv, NULL));

			break;
		}case T_On_RspQryInstrumentCommissionRate:
		{
			napi_value argv[4];
			pkg_cb_RspQryInstrumentCommissionRate(cbTrnField, argv);

			NAPI_CALL_RETURN_VOID(env_, napi_call_function(env_, global, cb, 4, argv, NULL));

			break;

		}default:
		{
			break;
		}
	}

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


void CTPTraderClient::OnRspError(CThostFtdcRspInfoField *pRspInfo, int nRequestID, bool bIsLast)
{
	CThostFtdcRspInfoField* _pRspInfo = NULL;
	if (pRspInfo) {
		_pRspInfo = new CThostFtdcRspInfoField();
		memcpy(_pRspInfo, pRspInfo, sizeof(CThostFtdcRspInfoField));
	}
	on_invoke(T_On_RspError, _pRspInfo, pRspInfo, nRequestID, bIsLast);
}

void CTPTraderClient::OnRspAuthenticate(CThostFtdcRspAuthenticateField *pRspAuthenticateField, CThostFtdcRspInfoField *pRspInfo, int nRequestID, bool bIsLast)
{
	CThostFtdcRspAuthenticateField *_pRspAuthenticateField = NULL;
	if (pRspAuthenticateField)
	{
		_pRspAuthenticateField = new CThostFtdcRspAuthenticateField();
		memcpy(_pRspAuthenticateField, pRspAuthenticateField, sizeof(CThostFtdcRspAuthenticateField));
	}

	on_invoke(T_On_RspAuthenticate, _pRspAuthenticateField, pRspInfo, nRequestID, bIsLast);
}

void CTPTraderClient::OnRspUserLogin(CThostFtdcRspUserLoginField *pRspUserLogin, CThostFtdcRspInfoField *pRspInfo, int nRequestID, bool bIsLast)
{
	CThostFtdcRspUserLoginField* _pRspUserLogin = NULL;
	if (pRspUserLogin) {
		_pRspUserLogin = new CThostFtdcRspUserLoginField();
		memcpy(_pRspUserLogin, pRspUserLogin, sizeof(CThostFtdcRspUserLoginField));
	}

	on_invoke(T_On_RspUserLogin, _pRspUserLogin, pRspInfo, nRequestID, bIsLast);
}

void CTPTraderClient::OnRspQrySettlementInfo(CThostFtdcSettlementInfoField *pSettlementInfo, CThostFtdcRspInfoField *pRspInfo, int nRequestID, bool bIsLast) 
{
	CThostFtdcSettlementInfoField* _pSettlementInfo = NULL;
	if (pSettlementInfo) {
		_pSettlementInfo = new CThostFtdcSettlementInfoField();
		memcpy(_pSettlementInfo, pSettlementInfo, sizeof(CThostFtdcSettlementInfoField));
	}

	on_invoke(T_On_RspQrySettlementInfo, _pSettlementInfo, pRspInfo, nRequestID, bIsLast);
}

void CTPTraderClient::OnRspSettlementInfoConfirm(CThostFtdcSettlementInfoConfirmField *pSettlementInfoConfirm, CThostFtdcRspInfoField *pRspInfo, int nRequestID, bool bIsLast)
{
	CThostFtdcSettlementInfoConfirmField *_pSettlementInfoConfirm = NULL;
	if (pSettlementInfoConfirm)
	{
		_pSettlementInfoConfirm = new CThostFtdcSettlementInfoConfirmField();
		memcpy(_pSettlementInfoConfirm, pSettlementInfoConfirm, sizeof(CThostFtdcSettlementInfoConfirmField));
	}

	on_invoke(T_On_RspSettlementInfoConfirm, _pSettlementInfoConfirm, pRspInfo, nRequestID, bIsLast);
}

void CTPTraderClient::OnRspQryInstrument(CThostFtdcInstrumentField *pInstrument, CThostFtdcRspInfoField *pRspInfo, int nRequestID, bool bIsLast)
{
	CThostFtdcInstrumentField *_pInstrument = NULL;
	if (pInstrument)
	{
		_pInstrument = new CThostFtdcInstrumentField();
		memcpy(_pInstrument, pInstrument, sizeof(CThostFtdcInstrumentField));
	}

	on_invoke(T_On_RspQryInstrument, _pInstrument, pRspInfo, nRequestID, bIsLast);
}

void  CTPTraderClient::OnRspQryTradingAccount(CThostFtdcTradingAccountField *pTradingAccount, CThostFtdcRspInfoField *pRspInfo, int nRequestID, bool bIsLast)
{
	CThostFtdcTradingAccountField *_pTradingAccount = NULL;
	if (pTradingAccount)
	{
		_pTradingAccount = new CThostFtdcTradingAccountField();
		memcpy(_pTradingAccount, pTradingAccount, sizeof(CThostFtdcTradingAccountField));
	}

	on_invoke(T_On_RspQryTradingAccount, _pTradingAccount, pRspInfo, nRequestID, bIsLast);
}

void CTPTraderClient::OnRspQryInvestorPosition(CThostFtdcInvestorPositionField *pInvestorPosition, CThostFtdcRspInfoField *pRspInfo, int nRequestID, bool bIsLast)
{

	CThostFtdcInvestorPositionField *_pInvestorPosition = NULL;
	if (pInvestorPosition)
	{
		_pInvestorPosition = new CThostFtdcInvestorPositionField();
		memcpy(_pInvestorPosition, pInvestorPosition, sizeof(CThostFtdcInvestorPositionField));
	}

	on_invoke(T_On_RspQryInvestorPosition, _pInvestorPosition, pRspInfo, nRequestID, bIsLast);
}

void  CTPTraderClient::OnRspOrderInsert(CThostFtdcInputOrderField *pInputOrder, CThostFtdcRspInfoField *pRspInfo, int nRequestID, bool bIsLast)
{
	CThostFtdcInputOrderField *_pInputOrder = NULL;
	if (pInputOrder)
	{
		_pInputOrder = new CThostFtdcInputOrderField();
		memcpy(_pInputOrder, pInputOrder, sizeof(CThostFtdcInputOrderField));
	}

	on_invoke(T_On_RspOrderInsert, _pInputOrder, pRspInfo, nRequestID, bIsLast);
}

void  CTPTraderClient::OnErrRtnOrderInsert(CThostFtdcInputOrderField *pInputOrder, CThostFtdcRspInfoField *pRspInfo)
{
	CThostFtdcInputOrderField *_pInputOrder = NULL;
	if (pInputOrder)
	{
		_pInputOrder = new CThostFtdcInputOrderField();
		memcpy(_pInputOrder, pInputOrder, sizeof(CThostFtdcInputOrderField));
	}

	on_invoke(T_On_ErrRtnOrderInsert, _pInputOrder, pRspInfo, 0, 0);
}

void CTPTraderClient::OnRtnOrder(CThostFtdcOrderField *pOrder)
{
	CThostFtdcOrderField *_pOrder = NULL;
	if (pOrder)
	{
		_pOrder = new CThostFtdcOrderField();
		memcpy(_pOrder, pOrder, sizeof(CThostFtdcOrderField));
	}

	on_invoke(T_On_RtnOrder, _pOrder, new CThostFtdcRspInfoField(), 0, 0);
}

void CTPTraderClient::OnRtnTrade(CThostFtdcTradeField *pTrade)
{
	CThostFtdcTradeField *_pTrade = NULL;
	if (pTrade)
	{
		_pTrade = new CThostFtdcTradeField();
		memcpy(_pTrade, pTrade, sizeof(CThostFtdcTradeField));
	}

	on_invoke(T_On_RtnTrade, _pTrade, new CThostFtdcRspInfoField(), 0, 0);
}

void CTPTraderClient::OnRspOrderAction(CThostFtdcInputOrderActionField *pInputOrderAction, CThostFtdcRspInfoField *pRspInfo, int nRequestID, bool bIsLast)
{
	CThostFtdcInputOrderActionField *_pInputOrderAction = NULL;
	if (pInputOrderAction)
	{
		_pInputOrderAction = new CThostFtdcInputOrderActionField();
		memcpy(_pInputOrderAction, pInputOrderAction, sizeof(CThostFtdcInputOrderActionField));
	}

	on_invoke(T_On_RspOrderAction, _pInputOrderAction, pRspInfo, nRequestID, bIsLast);
}

void CTPTraderClient::OnErrRtnOrderAction(CThostFtdcOrderActionField *pOrderAction, CThostFtdcRspInfoField *pRspInfo)
{
	CThostFtdcOrderActionField *_pOrderAction = NULL;
	if (pOrderAction)
	{
		_pOrderAction = new CThostFtdcOrderActionField();
		memcpy(_pOrderAction, pOrderAction, sizeof(CThostFtdcOrderActionField));
	}

	on_invoke(T_On_ErrRtnOrderAction, _pOrderAction, pRspInfo, 0, 0);
}

void CTPTraderClient::OnRspUserLogout(CThostFtdcUserLogoutField *pUserLogout, CThostFtdcRspInfoField *pRspInfo, int nRequestID, bool bIsLast)
{
	CThostFtdcUserLogoutField *_pUserLogout = NULL;
	if (pUserLogout)
	{
		_pUserLogout = new CThostFtdcUserLogoutField();
		memcpy(_pUserLogout, pUserLogout, sizeof(CThostFtdcUserLogoutField));
	}

	on_invoke(T_On_RspUserLogout, _pUserLogout, pRspInfo, nRequestID, bIsLast);
}


void CTPTraderClient::OnRspQryInstrumentCommissionRate(CThostFtdcInstrumentCommissionRateField *pInstrumentCommissionRate, CThostFtdcRspInfoField *pRspInfo, int nRequestID, bool bIsLast)
{
	CThostFtdcInstrumentCommissionRateField *_pInstrumentCommissionRate = NULL;
	if (pInstrumentCommissionRate)
	{
		_pInstrumentCommissionRate = new CThostFtdcInstrumentCommissionRateField();
		memcpy(_pInstrumentCommissionRate, pInstrumentCommissionRate, sizeof(CThostFtdcInstrumentCommissionRateField));
	}

	on_invoke(T_On_RspQryInstrumentCommissionRate, _pInstrumentCommissionRate, pRspInfo, nRequestID, bIsLast);
}


void CTPTraderClient::pkg_cb_rspuserlogout(OnEventCbRtnField* data, napi_value* cbArgs)
{
	CThostFtdcUserLogoutField *pUserLogout = static_cast<CThostFtdcUserLogoutField*>(data->rtnField);
	if (pUserLogout)
	{
		NAPI_CALL_RETURN_VOID(env_, napi_create_object(env_, cbArgs));

		napi_value brokerID;
		NAPI_CALL_RETURN_VOID(env_, napi_create_string_utf8(env_, pUserLogout->BrokerID,
			NAPI_AUTO_LENGTH, &brokerID));

		napi_value userID;
		NAPI_CALL_RETURN_VOID(env_, napi_create_string_utf8(env_, pUserLogout->UserID,
			NAPI_AUTO_LENGTH, &userID));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "BrokerID", brokerID));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "UserID", userID));

	}
	else
	{
		NAPI_CALL_RETURN_VOID(env_, napi_get_undefined(env_, cbArgs));
	}

	CThostFtdcRspInfoField *pRspInfo = static_cast<CThostFtdcRspInfoField*>(data->rspInfo);

	pkg_rspinfo(pRspInfo, cbArgs + 1);

	NAPI_CALL_RETURN_VOID(env_, napi_create_int32(env_, data->nRequestID, (cbArgs + 2)));

	NAPI_CALL_RETURN_VOID(env_, napi_get_boolean(env_, data->bIsLast, (cbArgs + 3)));

}

void CTPTraderClient::pkg_cb_errrtnorderaction(OnEventCbRtnField* data, napi_value* cbArgs)
{
	CThostFtdcOrderActionField *pOrderAction = static_cast<CThostFtdcOrderActionField*>(data->rtnField);
	if (pOrderAction)
	{

		NAPI_CALL_RETURN_VOID(env_, napi_create_object(env_, cbArgs));

		///经纪公司代码
		//TThostFtdcBrokerIDType	BrokerID;
		//typedef char TThostFtdcBrokerIDType[11];
		napi_value BrokerID;
		NAPI_CALL_RETURN_VOID(env_, napi_create_string_utf8(env_, pOrderAction->BrokerID,
			NAPI_AUTO_LENGTH, &BrokerID));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "BrokerID", BrokerID));
		
		///投资者代码
		//TThostFtdcInvestorIDType	InvestorID;
		//typedef char TThostFtdcInvestorIDType[13];

		napi_value InvestorID;
		NAPI_CALL_RETURN_VOID(env_, napi_create_string_utf8(env_, pOrderAction->InvestorID,
			NAPI_AUTO_LENGTH, &InvestorID));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "InvestorID", InvestorID));
		
		///报单操作引用
		//TThostFtdcOrderActionRefType	OrderActionRef;
		//typedef int TThostFtdcOrderActionRefType;
		napi_value OrderActionRef;
		NAPI_CALL_RETURN_VOID(env_, napi_create_int32(env_, pOrderAction->OrderActionRef,&OrderActionRef));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "OrderActionRef", OrderActionRef));
		
		///报单引用
		//TThostFtdcOrderRefType	OrderRef;
		//typedef char TThostFtdcOrderRefType[13];
		napi_value OrderRef;
		NAPI_CALL_RETURN_VOID(env_, napi_create_string_utf8(env_, pOrderAction->OrderRef,
			NAPI_AUTO_LENGTH, &OrderRef));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "OrderRef", OrderRef));

		
		///请求编号
		//TThostFtdcRequestIDType	RequestID;
		//typedef int TThostFtdcRequestIDType;
		napi_value RequestID;
		NAPI_CALL_RETURN_VOID(env_, napi_create_int32(env_, pOrderAction->RequestID, &RequestID));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "RequestID", RequestID));

		///前置编号
		//TThostFtdcFrontIDType	FrontID;
		//typedef int TThostFtdcFrontIDType;
		napi_value FrontID;
		NAPI_CALL_RETURN_VOID(env_, napi_create_int32(env_, pOrderAction->FrontID, &FrontID));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "FrontID", FrontID));

		///会话编号
		//TThostFtdcSessionIDType	SessionID;
		//typedef int TThostFtdcSessionIDType;
		napi_value SessionID;
		NAPI_CALL_RETURN_VOID(env_, napi_create_int32(env_, pOrderAction->SessionID, &SessionID));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "SessionID", SessionID));
		
		///交易所代码
		//TThostFtdcExchangeIDType	ExchangeID;
		//typedef char TThostFtdcExchangeIDType[9];
		napi_value ExchangeID;
		NAPI_CALL_RETURN_VOID(env_, napi_create_string_utf8(env_, pOrderAction->ExchangeID,
			NAPI_AUTO_LENGTH, &ExchangeID));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "ExchangeID", ExchangeID));

		
		///报单编号
		//TThostFtdcOrderSysIDType	OrderSysID;
		//typedef char TThostFtdcOrderSysIDType[21];
		napi_value OrderSysID;
		NAPI_CALL_RETURN_VOID(env_, napi_create_string_utf8(env_, pOrderAction->OrderSysID,
			NAPI_AUTO_LENGTH, &OrderSysID));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "OrderSysID", OrderSysID));


		///操作标志
		//TThostFtdcActionFlagType	ActionFlag;
		//typedef char TThostFtdcActionFlagType;
		napi_value ActionFlag;
		if (pOrderAction->ActionFlag != '\0') {
			NAPI_CALL_RETURN_VOID(env_, napi_create_string_utf8(env_, &pOrderAction->ActionFlag, 1, &ActionFlag));
		}
		else {
			NAPI_CALL_RETURN_VOID(env_, napi_create_string_utf8(env_, &pOrderAction->ActionFlag, 0, &ActionFlag));
		}
		
		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "ActionFlag", ActionFlag));

		///价格
		//TThostFtdcPriceType	LimitPrice;
		//typedef double TThostFtdcPriceType;
		napi_value LimitPrice;
		NAPI_CALL_RETURN_VOID(env_, napi_create_double(env_, pOrderAction->LimitPrice, &LimitPrice));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "LimitPrice", LimitPrice));

		
		///数量变化
		//TThostFtdcVolumeType	VolumeChange;
		//typedef int TThostFtdcVolumeType;
		napi_value VolumeChange;
		NAPI_CALL_RETURN_VOID(env_, napi_create_int32(env_, pOrderAction->VolumeChange, &VolumeChange));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "VolumeChange", VolumeChange));

		///操作日期
		//TThostFtdcDateType	ActionDate;
		//typedef char TThostFtdcDateType[9];
		napi_value ActionDate;
		NAPI_CALL_RETURN_VOID(env_, napi_create_string_utf8(env_, pOrderAction->ActionDate,
			NAPI_AUTO_LENGTH, &ActionDate));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "ActionDate", ActionDate));
		
		
		///操作时间
		//TThostFtdcTimeType	ActionTime;
		//typedef char TThostFtdcTimeType[9];
		napi_value ActionTime;
		NAPI_CALL_RETURN_VOID(env_, napi_create_string_utf8(env_, pOrderAction->ActionTime,
			NAPI_AUTO_LENGTH, &ActionTime));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "ActionTime", ActionTime));

		
		
		///交易所交易员代码
		//TThostFtdcTraderIDType	TraderID;
		//typedef char TThostFtdcTraderIDType[21];
		napi_value TraderID;
		NAPI_CALL_RETURN_VOID(env_, napi_create_string_utf8(env_, pOrderAction->TraderID,
			NAPI_AUTO_LENGTH, &TraderID));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "TraderID", TraderID));

		
		///安装编号
		//TThostFtdcInstallIDType	InstallID;
		//typedef int TThostFtdcInstallIDType;
		napi_value InstallID;
		NAPI_CALL_RETURN_VOID(env_, napi_create_int32(env_, pOrderAction->InstallID, &InstallID));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "InstallID", InstallID));

		
		///本地报单编号
		//TThostFtdcOrderLocalIDType	OrderLocalID;
		//typedef char TThostFtdcOrderLocalIDType[13];
		napi_value OrderLocalID;
		NAPI_CALL_RETURN_VOID(env_, napi_create_string_utf8(env_, pOrderAction->OrderLocalID,
			NAPI_AUTO_LENGTH, &OrderLocalID));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "OrderLocalID", OrderLocalID));
		
		///操作本地编号
		//TThostFtdcOrderLocalIDType	ActionLocalID;
		//typedef char TThostFtdcOrderLocalIDType[13];
		napi_value ActionLocalID;
		NAPI_CALL_RETURN_VOID(env_, napi_create_string_utf8(env_, pOrderAction->ActionLocalID,
			NAPI_AUTO_LENGTH, &ActionLocalID));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "ActionLocalID", ActionLocalID));

		
		///会员代码
		//TThostFtdcParticipantIDType	ParticipantID;
		//typedef char TThostFtdcParticipantIDType[11];
		napi_value ParticipantID;
		NAPI_CALL_RETURN_VOID(env_, napi_create_string_utf8(env_, pOrderAction->ParticipantID,
			NAPI_AUTO_LENGTH, &ParticipantID));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "ParticipantID", ParticipantID));

		
		///客户代码
		//TThostFtdcClientIDType	ClientID;
		//typedef char TThostFtdcClientIDType[11];
		napi_value ClientID;
		NAPI_CALL_RETURN_VOID(env_, napi_create_string_utf8(env_, pOrderAction->ClientID,
			NAPI_AUTO_LENGTH, &ClientID));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "ClientID", ClientID));

		///业务单元
		//TThostFtdcBusinessUnitType	BusinessUnit;
		//typedef char TThostFtdcBusinessUnitType[21];
		napi_value BusinessUnit;
		NAPI_CALL_RETURN_VOID(env_, napi_create_string_utf8(env_, pOrderAction->BusinessUnit,
			NAPI_AUTO_LENGTH, &BusinessUnit));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "BusinessUnit", BusinessUnit));


		///报单操作状态
		//TThostFtdcOrderActionStatusType	OrderActionStatus;
		//typedef char TThostFtdcOrderActionStatusType;
		napi_value OrderActionStatus;

		if (pOrderAction->OrderActionStatus != '\0') {
			NAPI_CALL_RETURN_VOID(env_, napi_create_string_utf8(env_, &pOrderAction->OrderActionStatus, 1, &OrderActionStatus));
		}
		else {
			NAPI_CALL_RETURN_VOID(env_, napi_create_string_utf8(env_, &pOrderAction->OrderActionStatus, 0, &OrderActionStatus));
		}

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "OrderActionStatus", OrderActionStatus));
		
		///用户代码
		//TThostFtdcUserIDType	UserID;
		//typedef char TThostFtdcUserIDType[16];
		napi_value UserID;
		NAPI_CALL_RETURN_VOID(env_, napi_create_string_utf8(env_, pOrderAction->UserID,
			NAPI_AUTO_LENGTH, &UserID));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "UserID", UserID));

		
		///状态信息
		//TThostFtdcErrorMsgType	StatusMsg;
		//typedef char TThostFtdcErrorMsgType[81];
		//单字符汉字char数组->utf8字符串
		//string MsgStr(pOrderAction->StatusMsg);
		string MsgUTF8 = CHString_To_UTF8(pOrderAction->StatusMsg);
		napi_value StatusMsg;
		NAPI_CALL_RETURN_VOID(env_, napi_create_string_utf8(env_, MsgUTF8.c_str(),
			NAPI_AUTO_LENGTH, &StatusMsg));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "StatusMsg", StatusMsg));
		
		///合约代码
		//TThostFtdcInstrumentIDType	InstrumentID;
		//typedef char TThostFtdcInstrumentIDType[31];
		napi_value InstrumentID;
		NAPI_CALL_RETURN_VOID(env_, napi_create_string_utf8(env_, pOrderAction->InstrumentID,
			NAPI_AUTO_LENGTH, &InstrumentID));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "InstrumentID", InstrumentID));

		///营业部编号
		//TThostFtdcBranchIDType	BranchID;
		//typedef char TThostFtdcBranchIDType[9];
		napi_value BranchID;
		NAPI_CALL_RETURN_VOID(env_, napi_create_string_utf8(env_, pOrderAction->BranchID,
			NAPI_AUTO_LENGTH, &BranchID));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "BranchID", BranchID));
		
		///投资单元代码
		//TThostFtdcInvestUnitIDType	InvestUnitID;
		//typedef char TThostFtdcInvestUnitIDType[17];
		napi_value InvestUnitID;
		NAPI_CALL_RETURN_VOID(env_, napi_create_string_utf8(env_, pOrderAction->InvestUnitID,
			NAPI_AUTO_LENGTH, &InvestUnitID));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "InvestUnitID", InvestUnitID));
		
		///IP地址
		//TThostFtdcIPAddressType	IPAddress;
		//typedef char TThostFtdcIPAddressType[16];
		napi_value IPAddress;
		NAPI_CALL_RETURN_VOID(env_, napi_create_string_utf8(env_, pOrderAction->IPAddress,
			NAPI_AUTO_LENGTH, &IPAddress));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "IPAddress", IPAddress));

		///Mac地址
		//TThostFtdcMacAddressType	MacAddress;
		//typedef char TThostFtdcMacAddressType[21];
		napi_value MacAddress;
		NAPI_CALL_RETURN_VOID(env_, napi_create_string_utf8(env_, pOrderAction->MacAddress,
			NAPI_AUTO_LENGTH, &MacAddress));
		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "MacAddress", MacAddress));
	}
	else
	{
		NAPI_CALL_RETURN_VOID(env_, napi_get_undefined(env_, cbArgs));
	}

	CThostFtdcRspInfoField *pRspInfo = static_cast<CThostFtdcRspInfoField*>(data->rspInfo);

	pkg_rspinfo(pRspInfo, cbArgs + 1);

}

void CTPTraderClient::pkg_cb_rsporderaction(OnEventCbRtnField* data, napi_value* cbArgs)
{
	CThostFtdcInputOrderActionField *pInputOrderAction = static_cast<CThostFtdcInputOrderActionField*>(data->rtnField);

	if (pInputOrderAction)
	{
		NAPI_CALL_RETURN_VOID(env_, napi_create_object(env_, cbArgs));


		///经纪公司代码
		//TThostFtdcBrokerIDType	BrokerID;
		//typedef char TThostFtdcBrokerIDType[11];
		napi_value BrokerID;
		NAPI_CALL_RETURN_VOID(env_, napi_create_string_utf8(env_, pInputOrderAction->BrokerID,
			NAPI_AUTO_LENGTH, &BrokerID));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "BrokerID", BrokerID));

		
		///投资者代码
		//TThostFtdcInvestorIDType	InvestorID;
		//typedef char TThostFtdcInvestorIDType[13];
		napi_value InvestorID;
		NAPI_CALL_RETURN_VOID(env_, napi_create_string_utf8(env_, pInputOrderAction->InvestorID,
			NAPI_AUTO_LENGTH, &InvestorID));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "InvestorID", InvestorID));

		///报单操作引用
		//TThostFtdcOrderActionRefType	OrderActionRef;
		//typedef int TThostFtdcOrderActionRefType;
		napi_value OrderActionRef;
		NAPI_CALL_RETURN_VOID(env_, napi_create_int32(env_, pInputOrderAction->OrderActionRef, &OrderActionRef));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "OrderActionRef", OrderActionRef));

		///报单引用
		//TThostFtdcOrderRefType	OrderRef;
		//typedef char TThostFtdcOrderRefType[13];
		napi_value OrderRef;
		NAPI_CALL_RETURN_VOID(env_, napi_create_string_utf8(env_, pInputOrderAction->OrderRef,
			NAPI_AUTO_LENGTH, &OrderRef));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "OrderRef", OrderRef));
		
		///请求编号
		//TThostFtdcRequestIDType	RequestID;
		//typedef int TThostFtdcRequestIDType;
		napi_value RequestID;
		NAPI_CALL_RETURN_VOID(env_, napi_create_int32(env_, pInputOrderAction->RequestID, &RequestID));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "RequestID", RequestID));


		///前置编号
		//TThostFtdcFrontIDType	FrontID;
		//typedef int TThostFtdcFrontIDType;
		napi_value FrontID;
		NAPI_CALL_RETURN_VOID(env_, napi_create_int32(env_, pInputOrderAction->FrontID, &FrontID));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "FrontID", FrontID));

		///会话编号
		//TThostFtdcSessionIDType	SessionID;
		//typedef int TThostFtdcSessionIDType;
		napi_value SessionID;
		NAPI_CALL_RETURN_VOID(env_, napi_create_int32(env_, pInputOrderAction->SessionID, &SessionID));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "SessionID", SessionID));

		///交易所代码
		//TThostFtdcExchangeIDType	ExchangeID;
		//typedef char TThostFtdcExchangeIDType[9];
		napi_value ExchangeID;
		NAPI_CALL_RETURN_VOID(env_, napi_create_string_utf8(env_, pInputOrderAction->ExchangeID,
			NAPI_AUTO_LENGTH, &ExchangeID));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "ExchangeID", ExchangeID));


		///报单编号
		//TThostFtdcOrderSysIDType	OrderSysID;
		//typedef char TThostFtdcOrderSysIDType[21];
		napi_value OrderSysID;
		NAPI_CALL_RETURN_VOID(env_, napi_create_string_utf8(env_, pInputOrderAction->OrderSysID,
			NAPI_AUTO_LENGTH, &OrderSysID));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "OrderSysID", OrderSysID));


		///操作标志
		//TThostFtdcActionFlagType	ActionFlag;
		//typedef char TThostFtdcActionFlagType;
		napi_value ActionFlag;

		if (pInputOrderAction->ActionFlag != '\0') {
			NAPI_CALL_RETURN_VOID(env_, napi_create_string_utf8(env_, &pInputOrderAction->ActionFlag, 1, &ActionFlag));
		}
		else {
			NAPI_CALL_RETURN_VOID(env_, napi_create_string_utf8(env_, &pInputOrderAction->ActionFlag, 0, &ActionFlag));
		}


		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "ActionFlag", ActionFlag));

		///价格
		//TThostFtdcPriceType	LimitPrice;
		//typedef double TThostFtdcPriceType;
		napi_value LimitPrice;
		NAPI_CALL_RETURN_VOID(env_, napi_create_double(env_, pInputOrderAction->LimitPrice, &LimitPrice));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "LimitPrice", LimitPrice));


		///数量变化
		//TThostFtdcVolumeType	VolumeChange;
		//typedef int TThostFtdcVolumeType;
		napi_value VolumeChange;
		NAPI_CALL_RETURN_VOID(env_, napi_create_int32(env_, pInputOrderAction->VolumeChange, &VolumeChange));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "VolumeChange", VolumeChange));


		///用户代码
		//TThostFtdcUserIDType	UserID;
		//typedef char TThostFtdcUserIDType[16];
		napi_value UserID;
		NAPI_CALL_RETURN_VOID(env_, napi_create_string_utf8(env_, pInputOrderAction->UserID,
			NAPI_AUTO_LENGTH, &UserID));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "UserID", UserID));

		///合约代码
		//TThostFtdcInstrumentIDType	InstrumentID;
		//typedef char TThostFtdcInstrumentIDType[31];
		napi_value InstrumentID;
		NAPI_CALL_RETURN_VOID(env_, napi_create_string_utf8(env_, pInputOrderAction->InstrumentID,
			NAPI_AUTO_LENGTH, &InstrumentID));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "InstrumentID", InstrumentID));

		///投资单元代码
		//TThostFtdcInvestUnitIDType	InvestUnitID;
		//typedef char TThostFtdcInvestUnitIDType[17];
		napi_value InvestUnitID;
		NAPI_CALL_RETURN_VOID(env_, napi_create_string_utf8(env_, pInputOrderAction->InvestUnitID,
			NAPI_AUTO_LENGTH, &InvestUnitID));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "InvestUnitID", InvestUnitID));

		///IP地址
		//TThostFtdcIPAddressType	IPAddress;
		//typedef char TThostFtdcIPAddressType[16];
		napi_value IPAddress;
		NAPI_CALL_RETURN_VOID(env_, napi_create_string_utf8(env_, pInputOrderAction->IPAddress,
			NAPI_AUTO_LENGTH, &IPAddress));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "IPAddress", IPAddress));

		///Mac地址
		//TThostFtdcMacAddressType	MacAddress;
		//typedef char TThostFtdcMacAddressType[21];
		napi_value MacAddress;
		NAPI_CALL_RETURN_VOID(env_, napi_create_string_utf8(env_, pInputOrderAction->MacAddress,
			NAPI_AUTO_LENGTH, &MacAddress));
		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "MacAddress", MacAddress));

	}
	else
	{
		NAPI_CALL_RETURN_VOID(env_, napi_get_undefined(env_, cbArgs));
	}

	CThostFtdcRspInfoField *pRspInfo = static_cast<CThostFtdcRspInfoField*>(data->rspInfo);
	pkg_rspinfo(pRspInfo, cbArgs + 1);

	NAPI_CALL_RETURN_VOID(env_, napi_create_int32(env_, data->nRequestID, (cbArgs + 2)));

	NAPI_CALL_RETURN_VOID(env_, napi_get_boolean(env_, data->bIsLast, (cbArgs + 3)));

}


void CTPTraderClient::pkg_cb_rtntrade(OnEventCbRtnField* data, napi_value* cbArgs)
{
	CThostFtdcTradeField *pTrade = static_cast<CThostFtdcTradeField*>(data->rtnField);

	if (pTrade)
	{
		NAPI_CALL_RETURN_VOID(env_, napi_create_object(env_, cbArgs));

		///经纪公司代码
		//TThostFtdcBrokerIDType	BrokerID;
		//typedef char TThostFtdcBrokerIDType[11];
		napi_value BrokerID;
		NAPI_CALL_RETURN_VOID(env_, napi_create_string_utf8(env_, pTrade->BrokerID,
			NAPI_AUTO_LENGTH, &BrokerID));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "BrokerID", BrokerID));

		///投资者代码
		//TThostFtdcInvestorIDType	InvestorID;
		//typedef char TThostFtdcInvestorIDType[13];
		napi_value InvestorID;
		NAPI_CALL_RETURN_VOID(env_, napi_create_string_utf8(env_, pTrade->InvestorID,
			NAPI_AUTO_LENGTH, &InvestorID));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "InvestorID", InvestorID));

		///合约代码
		//TThostFtdcInstrumentIDType	InstrumentID;
		//typedef char TThostFtdcInstrumentIDType[31];
		napi_value InstrumentID;
		NAPI_CALL_RETURN_VOID(env_, napi_create_string_utf8(env_, pTrade->InstrumentID,
			NAPI_AUTO_LENGTH, &InstrumentID));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "InstrumentID", InstrumentID));

		///报单引用
		//TThostFtdcOrderRefType	OrderRef;
		//typedef char TThostFtdcOrderRefType[13];
		napi_value OrderRef;
		NAPI_CALL_RETURN_VOID(env_, napi_create_string_utf8(env_, pTrade->OrderRef,
			NAPI_AUTO_LENGTH, &OrderRef));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "OrderRef", OrderRef));

		///用户代码
		//TThostFtdcUserIDType	UserID;
		//typedef char TThostFtdcUserIDType[16];
		napi_value UserID;
		NAPI_CALL_RETURN_VOID(env_, napi_create_string_utf8(env_, pTrade->UserID,
			NAPI_AUTO_LENGTH, &UserID));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "UserID", UserID));

		///交易所代码
		//TThostFtdcExchangeIDType	ExchangeID;
		//typedef char TThostFtdcExchangeIDType[9];
		napi_value ExchangeID;
		NAPI_CALL_RETURN_VOID(env_, napi_create_string_utf8(env_, pTrade->ExchangeID,
			NAPI_AUTO_LENGTH, &ExchangeID));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "ExchangeID", ExchangeID));

		
		///成交编号
		//TThostFtdcTradeIDType	TradeID;
		//typedef char TThostFtdcTradeIDType[21];
		napi_value TradeID;
		NAPI_CALL_RETURN_VOID(env_, napi_create_string_utf8(env_, pTrade->TradeID,
			NAPI_AUTO_LENGTH, &TradeID));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "TradeID", TradeID));

		
		///买卖方向
		//TThostFtdcDirectionType	Direction;
		//typedef char TThostFtdcDirectionType;
		napi_value Direction;

		if (pTrade->Direction != '\0') {
			NAPI_CALL_RETURN_VOID(env_, napi_create_string_utf8(env_, &pTrade->Direction, 1, &Direction));
		}
		else {
			NAPI_CALL_RETURN_VOID(env_, napi_create_string_utf8(env_, &pTrade->Direction, 0, &Direction));
		}

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "Direction", Direction));
		
		///报单编号
		//TThostFtdcOrderSysIDType	OrderSysID;
		//typedef char TThostFtdcOrderSysIDType[21];
		napi_value OrderSysID;
		NAPI_CALL_RETURN_VOID(env_, napi_create_string_utf8(env_, pTrade->OrderSysID,
			NAPI_AUTO_LENGTH, &OrderSysID));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "OrderSysID", OrderSysID));

		
		///会员代码
		//TThostFtdcParticipantIDType	ParticipantID;
		//typedef char TThostFtdcParticipantIDType[11];
		napi_value ParticipantID;
		NAPI_CALL_RETURN_VOID(env_, napi_create_string_utf8(env_, pTrade->ParticipantID,
			NAPI_AUTO_LENGTH, &ParticipantID));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "ParticipantID", ParticipantID));

		
		///客户代码
		//TThostFtdcClientIDType	ClientID;
		//typedef char TThostFtdcClientIDType[11];
		napi_value ClientID;
		NAPI_CALL_RETURN_VOID(env_, napi_create_string_utf8(env_, pTrade->ClientID,
			NAPI_AUTO_LENGTH, &ClientID));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "ClientID", ClientID));
		
		///交易角色
		//TThostFtdcTradingRoleType	TradingRole;
		//typedef char TThostFtdcTradingRoleType;
		napi_value TradingRole;

		if (pTrade->TradingRole != '\0') {
			NAPI_CALL_RETURN_VOID(env_, napi_create_string_utf8(env_, &pTrade->TradingRole, 1, &TradingRole));
		}
		else {
			NAPI_CALL_RETURN_VOID(env_, napi_create_string_utf8(env_, &pTrade->TradingRole, 0, &TradingRole));
		}

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "TradingRole", TradingRole));

		///合约在交易所的代码
		//TThostFtdcExchangeInstIDType	ExchangeInstID;
		//typedef char TThostFtdcExchangeInstIDType[31];
		napi_value ExchangeInstID;
		NAPI_CALL_RETURN_VOID(env_, napi_create_string_utf8(env_, pTrade->ExchangeInstID,
			NAPI_AUTO_LENGTH, &ExchangeInstID));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "ExchangeInstID", ExchangeInstID));

		///开平标志
		//TThostFtdcOffsetFlagType	OffsetFlag;
		//typedef char TThostFtdcOffsetFlagType;
		napi_value OffsetFlag;
		if (pTrade->OffsetFlag != '\0') {
			NAPI_CALL_RETURN_VOID(env_, napi_create_string_utf8(env_, &pTrade->OffsetFlag, 1, &OffsetFlag));
		}
		else {
			NAPI_CALL_RETURN_VOID(env_, napi_create_string_utf8(env_, &pTrade->OffsetFlag, 0, &OffsetFlag));
		}

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "OffsetFlag", OffsetFlag));

		
		///投机套保标志
		//TThostFtdcHedgeFlagType	HedgeFlag;
		//typedef char TThostFtdcHedgeFlagType;
		napi_value HedgeFlag;

		if (pTrade->HedgeFlag != '\0') {
			NAPI_CALL_RETURN_VOID(env_, napi_create_string_utf8(env_, &pTrade->HedgeFlag, 1, &HedgeFlag));
		}
		else {
			NAPI_CALL_RETURN_VOID(env_, napi_create_string_utf8(env_, &pTrade->HedgeFlag, 0, &HedgeFlag));
		}

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "HedgeFlag", HedgeFlag));

		
		///价格
		//TThostFtdcPriceType	Price;
		//typedef double TThostFtdcPriceType;
		napi_value Price;
		NAPI_CALL_RETURN_VOID(env_, napi_create_double(env_, pTrade->Price, &Price));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "Price", Price));

		
		///数量
		//TThostFtdcVolumeType	Volume;
		//typedef int TThostFtdcVolumeType;
		napi_value Volume;
		NAPI_CALL_RETURN_VOID(env_, napi_create_int32(env_, pTrade->Volume, &Volume));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "Volume", Volume));

		
		///成交时期
		//TThostFtdcDateType	TradeDate;
		//typedef char TThostFtdcDateType[9];
		napi_value TradeDate;
		NAPI_CALL_RETURN_VOID(env_, napi_create_string_utf8(env_, pTrade->TradeDate,
			NAPI_AUTO_LENGTH, &TradeDate));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "TradeDate", TradeDate));

		
		///成交时间
		//TThostFtdcTimeType	TradeTime;
		//typedef char TThostFtdcTimeType[9];
		napi_value TradeTime;
		NAPI_CALL_RETURN_VOID(env_, napi_create_string_utf8(env_, pTrade->TradeTime,
			NAPI_AUTO_LENGTH, &TradeTime));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "TradeTime", TradeTime));

		///成交类型
		//TThostFtdcTradeTypeType	TradeType;
		//typedef char TThostFtdcTradeTypeType;
		napi_value TradeType;

		if (pTrade->TradeType != '\0') {
			NAPI_CALL_RETURN_VOID(env_, napi_create_string_utf8(env_, &pTrade->TradeType, 1, &TradeType));
		}
		else {
			NAPI_CALL_RETURN_VOID(env_, napi_create_string_utf8(env_, &pTrade->TradeType, 0, &TradeType));
		}

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "TradeType", TradeType));

		///成交价来源
		//TThostFtdcPriceSourceType	PriceSource;
		//typedef char TThostFtdcPriceSourceType;
		napi_value PriceSource;

		if (pTrade->PriceSource != '\0') {
			NAPI_CALL_RETURN_VOID(env_, napi_create_string_utf8(env_, &pTrade->PriceSource, 1, &PriceSource));
		}
		else {
			NAPI_CALL_RETURN_VOID(env_, napi_create_string_utf8(env_, &pTrade->PriceSource, 0, &PriceSource));
		}

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "PriceSource", PriceSource));

		
		///交易所交易员代码
		//TThostFtdcTraderIDType	TraderID;
		//typedef char TThostFtdcTraderIDType[21];
		napi_value TraderID;
		NAPI_CALL_RETURN_VOID(env_, napi_create_string_utf8(env_, pTrade->TraderID,
			NAPI_AUTO_LENGTH, &TraderID));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "TraderID", TraderID));
		
		///本地报单编号
		//TThostFtdcOrderLocalIDType	OrderLocalID;
		//typedef char TThostFtdcOrderLocalIDType[13];
		napi_value OrderLocalID;
		NAPI_CALL_RETURN_VOID(env_, napi_create_string_utf8(env_, pTrade->OrderLocalID,
			NAPI_AUTO_LENGTH, &OrderLocalID));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "OrderLocalID", OrderLocalID));

		
		///结算会员编号
		//TThostFtdcParticipantIDType	ClearingPartID;
		//typedef char TThostFtdcParticipantIDType[11];
		napi_value ClearingPartID;
		NAPI_CALL_RETURN_VOID(env_, napi_create_string_utf8(env_, pTrade->ClearingPartID,
			NAPI_AUTO_LENGTH, &ClearingPartID));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "ClearingPartID", ClearingPartID));

		
		///业务单元
		//TThostFtdcBusinessUnitType	BusinessUnit;
		//typedef char TThostFtdcBusinessUnitType[21];
		napi_value BusinessUnit;
		NAPI_CALL_RETURN_VOID(env_, napi_create_string_utf8(env_, pTrade->BusinessUnit,
			NAPI_AUTO_LENGTH, &BusinessUnit));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "BusinessUnit", BusinessUnit));

		
		///序号
		//TThostFtdcSequenceNoType	SequenceNo;
		//typedef int TThostFtdcSequenceNoType;
		napi_value SequenceNo;
		NAPI_CALL_RETURN_VOID(env_, napi_create_int32(env_, pTrade->SequenceNo, &SequenceNo));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "SequenceNo", SequenceNo));

		
		///交易日
		//TThostFtdcDateType	TradingDay;
		//typedef char TThostFtdcDateType[9];
		napi_value TradingDay;
		NAPI_CALL_RETURN_VOID(env_, napi_create_string_utf8(env_, pTrade->TradingDay,
			NAPI_AUTO_LENGTH, &TradingDay));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "TradingDay", TradingDay));

		
		///结算编号
		//TThostFtdcSettlementIDType	SettlementID;
		//typedef int TThostFtdcSettlementIDType;
		napi_value SettlementID;
		NAPI_CALL_RETURN_VOID(env_, napi_create_int32(env_, pTrade->SettlementID, &SettlementID));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "SettlementID", SettlementID));

		
		///经纪公司报单编号
		//TThostFtdcSequenceNoType	BrokerOrderSeq;
		//typedef int TThostFtdcSequenceNoType;

		napi_value BrokerOrderSeq;
		NAPI_CALL_RETURN_VOID(env_, napi_create_int32(env_, pTrade->BrokerOrderSeq, &BrokerOrderSeq));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "BrokerOrderSeq", BrokerOrderSeq));

		
		///成交来源
		//TThostFtdcTradeSourceType	TradeSource;
		//typedef char TThostFtdcTradeSourceType;
		
		napi_value TradeSource;

		if (pTrade->TradeSource != '\0') {
			NAPI_CALL_RETURN_VOID(env_, napi_create_string_utf8(env_, &pTrade->TradeSource, 1, &TradeSource));
		}
		else {
			NAPI_CALL_RETURN_VOID(env_, napi_create_string_utf8(env_, &pTrade->TradeSource, 0, &TradeSource));
		}

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "TradeSource", TradeSource));

	}
	else
	{
		NAPI_CALL_RETURN_VOID(env_, napi_get_undefined(env_, cbArgs));
	}

}

void CTPTraderClient::pkg_cb_rtnorder(OnEventCbRtnField* data, napi_value* cbArgs)
{
	CThostFtdcOrderField *pOrder = static_cast<CThostFtdcOrderField*>(data->rtnField);
	if (pOrder)
	{

		NAPI_CALL_RETURN_VOID(env_, napi_create_object(env_, cbArgs));

		///经纪公司代码
		//TThostFtdcBrokerIDType	BrokerID;
		//typedef char TThostFtdcBrokerIDType[11];
		napi_value BrokerID;
		NAPI_CALL_RETURN_VOID(env_, napi_create_string_utf8(env_, pOrder->BrokerID,
			NAPI_AUTO_LENGTH, &BrokerID));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "BrokerID", BrokerID));

		///投资者代码
		//TThostFtdcInvestorIDType	InvestorID;
		//typedef char TThostFtdcInvestorIDType[13];
		napi_value InvestorID;
		NAPI_CALL_RETURN_VOID(env_, napi_create_string_utf8(env_, pOrder->InvestorID,
			NAPI_AUTO_LENGTH, &InvestorID));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "InvestorID", InvestorID));
		
		///合约代码
		//TThostFtdcInstrumentIDType	InstrumentID;
		//typedef char TThostFtdcInstrumentIDType[31];
		napi_value InstrumentID;
		NAPI_CALL_RETURN_VOID(env_, napi_create_string_utf8(env_, pOrder->InstrumentID,
			NAPI_AUTO_LENGTH, &InstrumentID));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "InstrumentID", InstrumentID));

		///报单引用
		//TThostFtdcOrderRefType	OrderRef;
		//typedef char TThostFtdcOrderRefType[13];
		napi_value OrderRef;
		NAPI_CALL_RETURN_VOID(env_, napi_create_string_utf8(env_, pOrder->OrderRef,
			NAPI_AUTO_LENGTH, &OrderRef));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "OrderRef", OrderRef));

		///用户代码
		//TThostFtdcUserIDType	UserID;
		//typedef char TThostFtdcUserIDType[16];
		napi_value UserID;
		NAPI_CALL_RETURN_VOID(env_, napi_create_string_utf8(env_, pOrder->UserID,
			NAPI_AUTO_LENGTH, &UserID));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "UserID", UserID));

		
		
		///报单价格条件
		//TThostFtdcOrderPriceTypeType	OrderPriceType;
		//typedef char TThostFtdcOrderPriceTypeType;
		napi_value OrderPriceType;

		if (pOrder->OrderPriceType != '\0') {
			NAPI_CALL_RETURN_VOID(env_, napi_create_string_utf8(env_, &pOrder->OrderPriceType, 1, &OrderPriceType));
		}
		else {
			NAPI_CALL_RETURN_VOID(env_, napi_create_string_utf8(env_, &pOrder->OrderPriceType, 0, &OrderPriceType));
		}

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "OrderPriceType", OrderPriceType));

		
		
		///买卖方向
		//TThostFtdcDirectionType	Direction;
		//typedef char TThostFtdcDirectionType;
		napi_value Direction;
		if (pOrder->Direction != '\0') {
			NAPI_CALL_RETURN_VOID(env_, napi_create_string_utf8(env_, &pOrder->Direction, 1, &Direction));
		}
		else {
			NAPI_CALL_RETURN_VOID(env_, napi_create_string_utf8(env_, &pOrder->Direction, 0, &Direction));
		}
		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "Direction", Direction));


		///组合开平标志
		//TThostFtdcCombOffsetFlagType	CombOffsetFlag;
		//typedef char TThostFtdcCombOffsetFlagType[5];
		napi_value CombOffsetFlag;
		NAPI_CALL_RETURN_VOID(env_, napi_create_string_utf8(env_, pOrder->CombOffsetFlag,
			NAPI_AUTO_LENGTH, &CombOffsetFlag));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "CombOffsetFlag", CombOffsetFlag));

		///组合投机套保标志
		//TThostFtdcCombHedgeFlagType	CombHedgeFlag;
		//typedef char TThostFtdcCombHedgeFlagType[5];
		napi_value CombHedgeFlag;
		NAPI_CALL_RETURN_VOID(env_, napi_create_string_utf8(env_, pOrder->CombHedgeFlag,
			NAPI_AUTO_LENGTH, &CombHedgeFlag));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "CombHedgeFlag", CombHedgeFlag));

		///价格
		//TThostFtdcPriceType	LimitPrice;
		//typedef double TThostFtdcPriceType;
		napi_value LimitPrice;
		NAPI_CALL_RETURN_VOID(env_, napi_create_double(env_, pOrder->LimitPrice, &LimitPrice));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "LimitPrice", LimitPrice));		
		
		///数量
		//TThostFtdcVolumeType	VolumeTotalOriginal;
		//typedef int TThostFtdcVolumeType;
		napi_value VolumeTotalOriginal;
		NAPI_CALL_RETURN_VOID(env_, napi_create_int32(env_, pOrder->VolumeTotalOriginal, &VolumeTotalOriginal));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "VolumeTotalOriginal", VolumeTotalOriginal));

		///有效期类型
		//TThostFtdcTimeConditionType	TimeCondition;
		//typedef char TThostFtdcTimeConditionType;
		napi_value TimeCondition;
		if (pOrder->TimeCondition != '\0') {
			NAPI_CALL_RETURN_VOID(env_, napi_create_string_utf8(env_, &pOrder->TimeCondition, 1, &TimeCondition));
		}
		else {
			NAPI_CALL_RETURN_VOID(env_, napi_create_string_utf8(env_, &pOrder->TimeCondition, 0, &TimeCondition));
		}
		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "TimeCondition", TimeCondition));

		///GTD日期
		//TThostFtdcDateType	GTDDate;
		//typedef char TThostFtdcDateType[9];
		napi_value GTDDate;
		NAPI_CALL_RETURN_VOID(env_, napi_create_string_utf8(env_, pOrder->GTDDate,
			NAPI_AUTO_LENGTH, &GTDDate));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "GTDDate", GTDDate));

		///成交量类型
		//TThostFtdcVolumeConditionType	VolumeCondition;
		//typedef char TThostFtdcVolumeConditionType;
		napi_value VolumeCondition;

		if (pOrder->VolumeCondition != '\0') {
			NAPI_CALL_RETURN_VOID(env_, napi_create_string_utf8(env_, &pOrder->VolumeCondition, 1, &VolumeCondition));
		}
		else {
			NAPI_CALL_RETURN_VOID(env_, napi_create_string_utf8(env_, &pOrder->VolumeCondition, 0, &VolumeCondition));
		}

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "VolumeCondition", VolumeCondition));

		
		///最小成交量
		//TThostFtdcVolumeType	MinVolume;
		//typedef int TThostFtdcVolumeType;
		napi_value MinVolume;
		NAPI_CALL_RETURN_VOID(env_, napi_create_int32(env_, pOrder->MinVolume, &MinVolume));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "MinVolume", MinVolume));

		///触发条件
		//TThostFtdcContingentConditionType	ContingentCondition;
		//typedef char TThostFtdcContingentConditionType;
		napi_value ContingentCondition;
		if (pOrder->ContingentCondition != '\0') {
			NAPI_CALL_RETURN_VOID(env_, napi_create_string_utf8(env_, &pOrder->ContingentCondition, 1, &ContingentCondition));
		}
		else {
			NAPI_CALL_RETURN_VOID(env_, napi_create_string_utf8(env_, &pOrder->ContingentCondition, 0, &ContingentCondition));
		}
		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "ContingentCondition", ContingentCondition));

		///止损价
		//TThostFtdcPriceType	StopPrice;
		//typedef double TThostFtdcPriceType;
		napi_value StopPrice;
		NAPI_CALL_RETURN_VOID(env_, napi_create_double(env_, pOrder->StopPrice, &StopPrice));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "StopPrice", StopPrice));

		///强平原因
		//TThostFtdcForceCloseReasonType	ForceCloseReason;
		//typedef char TThostFtdcForceCloseReasonType;
		napi_value ForceCloseReason;
		
		if (pOrder->ForceCloseReason != '\0') {
			NAPI_CALL_RETURN_VOID(env_, napi_create_string_utf8(env_, &pOrder->ForceCloseReason, 1, &ForceCloseReason));
		}
		else {
			NAPI_CALL_RETURN_VOID(env_, napi_create_string_utf8(env_, &pOrder->ForceCloseReason, 0, &ForceCloseReason));
		}

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "ForceCloseReason", ForceCloseReason));

		///自动挂起标志
		//TThostFtdcBoolType	IsAutoSuspend;
		//typedef int TThostFtdcBoolType;
		napi_value IsAutoSuspend;
		NAPI_CALL_RETURN_VOID(env_, napi_create_int32(env_, pOrder->IsAutoSuspend, &IsAutoSuspend));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "IsAutoSuspend", IsAutoSuspend));

		
		///业务单元
		//TThostFtdcBusinessUnitType	BusinessUnit;
		//typedef char TThostFtdcBusinessUnitType[21];
		napi_value BusinessUnit;
		NAPI_CALL_RETURN_VOID(env_, napi_create_string_utf8(env_, pOrder->BusinessUnit,
			NAPI_AUTO_LENGTH, &BusinessUnit));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "BusinessUnit", BusinessUnit));

		
		///请求编号
		//TThostFtdcRequestIDType	RequestID;
		//typedef int TThostFtdcRequestIDType;
		napi_value RequestID;
		NAPI_CALL_RETURN_VOID(env_, napi_create_int32(env_, pOrder->RequestID, &RequestID));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "RequestID", RequestID));

		
		///本地报单编号
		//TThostFtdcOrderLocalIDType	OrderLocalID;
		//typedef char TThostFtdcOrderLocalIDType[13];
		napi_value OrderLocalID;
		NAPI_CALL_RETURN_VOID(env_, napi_create_string_utf8(env_, pOrder->OrderLocalID,
			NAPI_AUTO_LENGTH, &OrderLocalID));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "OrderLocalID", OrderLocalID));


		///交易所代码
		//TThostFtdcExchangeIDType	ExchangeID;
		//typedef char TThostFtdcExchangeIDType[9];
		napi_value ExchangeID;
		NAPI_CALL_RETURN_VOID(env_, napi_create_string_utf8(env_, pOrder->ExchangeID,
			NAPI_AUTO_LENGTH, &ExchangeID));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "ExchangeID", ExchangeID));
		
		///会员代码
		//TThostFtdcParticipantIDType	ParticipantID;
		//typedef char TThostFtdcParticipantIDType[11];
		napi_value ParticipantID;
		NAPI_CALL_RETURN_VOID(env_, napi_create_string_utf8(env_, pOrder->ParticipantID,
			NAPI_AUTO_LENGTH, &ParticipantID));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "ParticipantID", ParticipantID));

		///客户代码
		//TThostFtdcClientIDType	ClientID;
		//typedef char TThostFtdcClientIDType[11];
		napi_value ClientID;
		NAPI_CALL_RETURN_VOID(env_, napi_create_string_utf8(env_, pOrder->ClientID,
			NAPI_AUTO_LENGTH, &ClientID));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "ClientID", ClientID));
		
		///合约在交易所的代码
		//TThostFtdcExchangeInstIDType	ExchangeInstID;
		//typedef char TThostFtdcExchangeInstIDType[31];
		napi_value ExchangeInstID;
		NAPI_CALL_RETURN_VOID(env_, napi_create_string_utf8(env_, pOrder->ExchangeInstID,
			NAPI_AUTO_LENGTH, &ExchangeInstID));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "ExchangeInstID", ExchangeInstID));
		
		///交易所交易员代码
		//TThostFtdcTraderIDType	TraderID;
		//typedef char TThostFtdcTraderIDType[21];
		napi_value TraderID;
		NAPI_CALL_RETURN_VOID(env_, napi_create_string_utf8(env_, pOrder->TraderID,
			NAPI_AUTO_LENGTH, &TraderID));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "TraderID", TraderID));
		
		///安装编号
		//TThostFtdcInstallIDType	InstallID;
		//typedef int TThostFtdcInstallIDType;
		napi_value InstallID;
		NAPI_CALL_RETURN_VOID(env_, napi_create_int32(env_, pOrder->InstallID, &InstallID));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "InstallID", InstallID));

		
		///报单提交状态
		//TThostFtdcOrderSubmitStatusType	OrderSubmitStatus;
		//typedef char TThostFtdcOrderSubmitStatusType;
		napi_value OrderSubmitStatus;

		if (pOrder->OrderSubmitStatus != '\0') {
			NAPI_CALL_RETURN_VOID(env_, napi_create_string_utf8(env_, &pOrder->OrderSubmitStatus, 1, &OrderSubmitStatus));
		}
		else {
			NAPI_CALL_RETURN_VOID(env_, napi_create_string_utf8(env_, &pOrder->OrderSubmitStatus, 0, &OrderSubmitStatus));
		}

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "OrderSubmitStatus", OrderSubmitStatus));

		///报单提示序号
		//TThostFtdcSequenceNoType	NotifySequence;
		//typedef int TThostFtdcSequenceNoType;
		napi_value NotifySequence;
		NAPI_CALL_RETURN_VOID(env_, napi_create_int32(env_, pOrder->NotifySequence, &NotifySequence));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "NotifySequence", NotifySequence));

		
		///交易日
		//TThostFtdcDateType	TradingDay;
		//typedef char TThostFtdcDateType[9];
		napi_value TradingDay;
		NAPI_CALL_RETURN_VOID(env_, napi_create_string_utf8(env_, pOrder->TradingDay,
			NAPI_AUTO_LENGTH, &TradingDay));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "TradingDay", TradingDay));

		///结算编号
		//TThostFtdcSettlementIDType	SettlementID;
		//typedef int TThostFtdcSettlementIDType;
		napi_value SettlementID;
		NAPI_CALL_RETURN_VOID(env_, napi_create_int32(env_, pOrder->SettlementID, &SettlementID));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "SettlementID", SettlementID));

		
		///报单编号
		//TThostFtdcOrderSysIDType	OrderSysID;
		//typedef char TThostFtdcOrderSysIDType[21];
		napi_value OrderSysID;
		NAPI_CALL_RETURN_VOID(env_, napi_create_string_utf8(env_, pOrder->OrderSysID,
			NAPI_AUTO_LENGTH, &OrderSysID));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "OrderSysID", OrderSysID));

		
		///报单来源
		//TThostFtdcOrderSourceType	OrderSource;
		//typedef char TThostFtdcOrderSourceType;
		napi_value OrderSource;
		
		if (pOrder->OrderSource != '\0') {
			NAPI_CALL_RETURN_VOID(env_, napi_create_string_utf8(env_, &pOrder->OrderSource, 1, &OrderSource));
		}
		else {
			NAPI_CALL_RETURN_VOID(env_, napi_create_string_utf8(env_, &pOrder->OrderSource, 0, &OrderSource));
		}

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "OrderSource", OrderSource));

		
		
		///报单状态
		//TThostFtdcOrderStatusType	OrderStatus;
		//typedef char TThostFtdcOrderStatusType;
		napi_value OrderStatus;

		if (pOrder->OrderStatus != '\0') {
			NAPI_CALL_RETURN_VOID(env_, napi_create_string_utf8(env_, &pOrder->OrderStatus, 1, &OrderStatus));
		}
		else {
			NAPI_CALL_RETURN_VOID(env_, napi_create_string_utf8(env_, &pOrder->OrderStatus, 0, &OrderStatus));
		}

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "OrderStatus", OrderStatus));

		
		///报单类型
		//TThostFtdcOrderTypeType	OrderType;
		//typedef char TThostFtdcOrderTypeType;
		napi_value OrderType;

		if (pOrder->OrderType != '\0') {
			NAPI_CALL_RETURN_VOID(env_, napi_create_string_utf8(env_, &pOrder->OrderType, 1, &OrderType));
		}
		else {
			NAPI_CALL_RETURN_VOID(env_, napi_create_string_utf8(env_, &pOrder->OrderType, 0, &OrderType));
		}

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "OrderType", OrderType));

		
		///今成交数量
		//TThostFtdcVolumeType	VolumeTraded;
		//typedef int TThostFtdcVolumeType;
		napi_value VolumeTraded;
		NAPI_CALL_RETURN_VOID(env_, napi_create_int32(env_, pOrder->VolumeTraded, &VolumeTraded));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "VolumeTraded", VolumeTraded));

		
		///剩余数量
		//TThostFtdcVolumeType	VolumeTotal;
		//typedef int TThostFtdcVolumeType;
		napi_value VolumeTotal;
		NAPI_CALL_RETURN_VOID(env_, napi_create_int32(env_, pOrder->VolumeTotal, &VolumeTotal));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "VolumeTotal", VolumeTotal));

		
		///报单日期
		//TThostFtdcDateType	InsertDate;
		//typedef char TThostFtdcDateType[9];
		napi_value InsertDate;
		NAPI_CALL_RETURN_VOID(env_, napi_create_string_utf8(env_, pOrder->InsertDate,
			NAPI_AUTO_LENGTH, &InsertDate));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "InsertDate", InsertDate));

		///委托时间
		//TThostFtdcTimeType	InsertTime;
		//typedef char TThostFtdcTimeType[9];
		napi_value InsertTime;
		NAPI_CALL_RETURN_VOID(env_, napi_create_string_utf8(env_, pOrder->InsertTime,
			NAPI_AUTO_LENGTH, &InsertTime));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "InsertTime", InsertTime));

		
		///激活时间
		//TThostFtdcTimeType	ActiveTime;
		//typedef char TThostFtdcTimeType[9];
		napi_value ActiveTime;
		NAPI_CALL_RETURN_VOID(env_, napi_create_string_utf8(env_, pOrder->ActiveTime,
			NAPI_AUTO_LENGTH, &ActiveTime));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "ActiveTime", ActiveTime));

		///挂起时间
		//TThostFtdcTimeType	SuspendTime;
		//typedef char TThostFtdcTimeType[9];
		napi_value SuspendTime;
		NAPI_CALL_RETURN_VOID(env_, napi_create_string_utf8(env_, pOrder->SuspendTime,
			NAPI_AUTO_LENGTH, &SuspendTime));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "SuspendTime", SuspendTime));

		///最后修改时间
		//TThostFtdcTimeType	UpdateTime;
		//typedef char TThostFtdcTimeType[9];
		napi_value UpdateTime;
		NAPI_CALL_RETURN_VOID(env_, napi_create_string_utf8(env_, pOrder->UpdateTime,
			NAPI_AUTO_LENGTH, &UpdateTime));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "UpdateTime", UpdateTime));

		///撤销时间
		//TThostFtdcTimeType	CancelTime;
		//typedef char TThostFtdcTimeType[9];
		napi_value CancelTime;
		NAPI_CALL_RETURN_VOID(env_, napi_create_string_utf8(env_, pOrder->CancelTime,
			NAPI_AUTO_LENGTH, &CancelTime));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "CancelTime", CancelTime));

		
		///最后修改交易所交易员代码
		//TThostFtdcTraderIDType	ActiveTraderID;
		//typedef char TThostFtdcTraderIDType[21];
		napi_value ActiveTraderID;
		NAPI_CALL_RETURN_VOID(env_, napi_create_string_utf8(env_, pOrder->ActiveTraderID,
			NAPI_AUTO_LENGTH, &ActiveTraderID));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "ActiveTraderID", ActiveTraderID));

		
		///结算会员编号
		//TThostFtdcParticipantIDType	ClearingPartID;
		//typedef char TThostFtdcParticipantIDType[11];
		napi_value ClearingPartID;
		NAPI_CALL_RETURN_VOID(env_, napi_create_string_utf8(env_, pOrder->ClearingPartID,
			NAPI_AUTO_LENGTH, &ClearingPartID));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "ClearingPartID", ClearingPartID));
		
		///序号
		//TThostFtdcSequenceNoType	SequenceNo;
		//typedef int TThostFtdcSequenceNoType;
		napi_value SequenceNo;
		NAPI_CALL_RETURN_VOID(env_, napi_create_int32(env_, pOrder->SequenceNo, &SequenceNo));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "SequenceNo", SequenceNo));

		
		///前置编号
		//TThostFtdcFrontIDType	FrontID;
		//typedef int TThostFtdcFrontIDType;
		napi_value FrontID;
		NAPI_CALL_RETURN_VOID(env_, napi_create_int32(env_, pOrder->FrontID, &FrontID));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "FrontID", FrontID));

		
		///会话编号
		//TThostFtdcSessionIDType	SessionID;
		//typedef int TThostFtdcSessionIDType;
		napi_value SessionID;
		NAPI_CALL_RETURN_VOID(env_, napi_create_int32(env_, pOrder->SessionID, &SessionID));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "SessionID", SessionID));

		
		///用户端产品信息
		//TThostFtdcProductInfoType	UserProductInfo;
		//typedef char TThostFtdcProductInfoType[11];
		napi_value UserProductInfo;
		NAPI_CALL_RETURN_VOID(env_, napi_create_string_utf8(env_, pOrder->UserProductInfo,
			NAPI_AUTO_LENGTH, &UserProductInfo));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "UserProductInfo", UserProductInfo));

		
		///状态信息
		//TThostFtdcErrorMsgType	StatusMsg;
		//typedef char TThostFtdcErrorMsgType[81];
		//string MsgStr(pOrder->StatusMsg);
		string MsgUTF8 = CHString_To_UTF8(pOrder->StatusMsg);
		napi_value StatusMsg;
		NAPI_CALL_RETURN_VOID(env_, napi_create_string_utf8(env_, MsgUTF8.c_str(),
			NAPI_AUTO_LENGTH, &StatusMsg));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "StatusMsg", StatusMsg));

		
		///用户强评标志
		//TThostFtdcBoolType	UserForceClose;
		//typedef int TThostFtdcBoolType;
		napi_value UserForceClose;
		NAPI_CALL_RETURN_VOID(env_, napi_create_int32(env_, pOrder->UserForceClose, &UserForceClose));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "UserForceClose", UserForceClose));
		
		///操作用户代码
		//TThostFtdcUserIDType	ActiveUserID;
		//typedef char TThostFtdcUserIDType[16];
		napi_value ActiveUserID;
		NAPI_CALL_RETURN_VOID(env_, napi_create_string_utf8(env_, pOrder->ActiveUserID,
			NAPI_AUTO_LENGTH, &ActiveUserID));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "ActiveUserID", ActiveUserID));

		///经纪公司报单编号
		//TThostFtdcSequenceNoType	BrokerOrderSeq;
		//typedef int TThostFtdcSequenceNoType;
		napi_value BrokerOrderSeq;
		NAPI_CALL_RETURN_VOID(env_, napi_create_int32(env_, pOrder->BrokerOrderSeq, &BrokerOrderSeq));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "BrokerOrderSeq", BrokerOrderSeq));
		
		///相关报单
		//TThostFtdcOrderSysIDType	RelativeOrderSysID;
		//typedef char TThostFtdcOrderSysIDType[21];
		napi_value RelativeOrderSysID;
		NAPI_CALL_RETURN_VOID(env_, napi_create_string_utf8(env_, pOrder->RelativeOrderSysID,
			NAPI_AUTO_LENGTH, &RelativeOrderSysID));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "RelativeOrderSysID", RelativeOrderSysID));

		
		///郑商所成交数量
		//TThostFtdcVolumeType	ZCETotalTradedVolume;
		//typedef int TThostFtdcVolumeType;
		napi_value ZCETotalTradedVolume;
		NAPI_CALL_RETURN_VOID(env_, napi_create_int32(env_, pOrder->ZCETotalTradedVolume, &ZCETotalTradedVolume));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "ZCETotalTradedVolume", ZCETotalTradedVolume));

		
		///互换单标志
		//TThostFtdcBoolType	IsSwapOrder;
		//typedef int TThostFtdcBoolType;
		napi_value IsSwapOrder;
		NAPI_CALL_RETURN_VOID(env_, napi_create_int32(env_, pOrder->IsSwapOrder, &IsSwapOrder));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "IsSwapOrder", IsSwapOrder));

		
		///营业部编号
		//TThostFtdcBranchIDType	BranchID;
		//typedef char TThostFtdcBranchIDType[9];
		napi_value BranchID;
		NAPI_CALL_RETURN_VOID(env_, napi_create_string_utf8(env_, pOrder->BranchID,
			NAPI_AUTO_LENGTH, &BranchID));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "BranchID", BranchID));

		
		///投资单元代码
		//TThostFtdcInvestUnitIDType	InvestUnitID;
		//typedef char TThostFtdcInvestUnitIDType[17];
		napi_value InvestUnitID;
		NAPI_CALL_RETURN_VOID(env_, napi_create_string_utf8(env_, pOrder->InvestUnitID,
			NAPI_AUTO_LENGTH, &InvestUnitID));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "InvestUnitID", InvestUnitID));

		///资金账号
		//TThostFtdcAccountIDType	AccountID;
		//typedef char TThostFtdcAccountIDType[13];
		napi_value AccountID;
		NAPI_CALL_RETURN_VOID(env_, napi_create_string_utf8(env_, pOrder->AccountID,
			NAPI_AUTO_LENGTH, &AccountID));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "AccountID", AccountID));

		
		///币种代码
		//TThostFtdcCurrencyIDType	CurrencyID;
		//typedef char TThostFtdcCurrencyIDType[4];
		napi_value CurrencyID;
		NAPI_CALL_RETURN_VOID(env_, napi_create_string_utf8(env_, pOrder->CurrencyID,
			NAPI_AUTO_LENGTH, &CurrencyID));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "CurrencyID", CurrencyID));

		///IP地址
		//TThostFtdcIPAddressType	IPAddress;
		//typedef char TThostFtdcIPAddressType[16];
		napi_value IPAddress;
		NAPI_CALL_RETURN_VOID(env_, napi_create_string_utf8(env_, pOrder->IPAddress,
			NAPI_AUTO_LENGTH, &IPAddress));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "IPAddress", IPAddress));

		///Mac地址
		//TThostFtdcMacAddressType	MacAddress;
		//typedef char TThostFtdcMacAddressType[21];
		napi_value MacAddress;
		NAPI_CALL_RETURN_VOID(env_, napi_create_string_utf8(env_, pOrder->MacAddress,
			NAPI_AUTO_LENGTH, &MacAddress));
		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "MacAddress", MacAddress));

	}
	else
	{
		NAPI_CALL_RETURN_VOID(env_, napi_get_undefined(env_, cbArgs));
	}
}

void CTPTraderClient::pkg_cb_errrtnorderinsert(OnEventCbRtnField* data, napi_value* cbArgs)
{
	CThostFtdcInputOrderField *pInputOrder = static_cast<CThostFtdcInputOrderField*>(data->rtnField);
	if (pInputOrder)
	{
		NAPI_CALL_RETURN_VOID(env_, napi_create_object(env_, cbArgs));

		///经纪公司代码
		//TThostFtdcBrokerIDType	BrokerID;
		//typedef char TThostFtdcBrokerIDType[11];
		napi_value BrokerID;
		NAPI_CALL_RETURN_VOID(env_, napi_create_string_utf8(env_, pInputOrder->BrokerID,
			NAPI_AUTO_LENGTH, &BrokerID));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "BrokerID", BrokerID));

		///投资者代码
		//TThostFtdcInvestorIDType	InvestorID;
		//typedef char TThostFtdcInvestorIDType[13];
		napi_value InvestorID;
		NAPI_CALL_RETURN_VOID(env_, napi_create_string_utf8(env_, pInputOrder->InvestorID,
			NAPI_AUTO_LENGTH, &InvestorID));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "InvestorID", InvestorID));

		///合约代码
		//TThostFtdcInstrumentIDType	InstrumentID;
		//typedef char TThostFtdcInstrumentIDType[31];
		napi_value InstrumentID;
		NAPI_CALL_RETURN_VOID(env_, napi_create_string_utf8(env_, pInputOrder->InstrumentID,
			NAPI_AUTO_LENGTH, &InstrumentID));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "InstrumentID", InstrumentID));

		///报单引用
		//TThostFtdcOrderRefType	OrderRef;
		//typedef char TThostFtdcOrderRefType[13];
		napi_value OrderRef;
		NAPI_CALL_RETURN_VOID(env_, napi_create_string_utf8(env_, pInputOrder->OrderRef,
			NAPI_AUTO_LENGTH, &OrderRef));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "OrderRef", OrderRef));

		///用户代码
		//TThostFtdcUserIDType	UserID;
		//typedef char TThostFtdcUserIDType[16];
		napi_value UserID;
		NAPI_CALL_RETURN_VOID(env_, napi_create_string_utf8(env_, pInputOrder->UserID,
			NAPI_AUTO_LENGTH, &UserID));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "UserID", UserID));
		
		///报单价格条件
		//TThostFtdcOrderPriceTypeType	OrderPriceType;
		//typedef char TThostFtdcOrderPriceTypeType;
		napi_value OrderPriceType;
		
		if (pInputOrder->OrderPriceType != '\0') {
			NAPI_CALL_RETURN_VOID(env_, napi_create_string_utf8(env_, &pInputOrder->OrderPriceType, 1, &OrderPriceType));
		}
		else {
			NAPI_CALL_RETURN_VOID(env_, napi_create_string_utf8(env_, &pInputOrder->OrderPriceType, 0, &OrderPriceType));
		}

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "OrderPriceType", OrderPriceType));



		///买卖方向
		//TThostFtdcDirectionType	Direction;
		//typedef char TThostFtdcDirectionType;
		napi_value Direction;
		if (pInputOrder->Direction != '\0') {
			NAPI_CALL_RETURN_VOID(env_, napi_create_string_utf8(env_, &pInputOrder->Direction, 1, &Direction));
		}
		else {
			NAPI_CALL_RETURN_VOID(env_, napi_create_string_utf8(env_, &pInputOrder->Direction, 0, &Direction));
		}
		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "Direction", Direction));

		
		///组合开平标志
		//TThostFtdcCombOffsetFlagType	CombOffsetFlag;
		//typedef char TThostFtdcCombOffsetFlagType[5];
		napi_value CombOffsetFlag;
		NAPI_CALL_RETURN_VOID(env_, napi_create_string_utf8(env_, pInputOrder->CombOffsetFlag,
			NAPI_AUTO_LENGTH, &CombOffsetFlag));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "CombOffsetFlag", CombOffsetFlag));

		///组合投机套保标志
		//TThostFtdcCombHedgeFlagType	CombHedgeFlag;
		//typedef char TThostFtdcCombHedgeFlagType[5];
		napi_value CombHedgeFlag;
		NAPI_CALL_RETURN_VOID(env_, napi_create_string_utf8(env_, pInputOrder->CombHedgeFlag,
			NAPI_AUTO_LENGTH, &CombHedgeFlag));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "CombHedgeFlag", CombHedgeFlag));

		///价格
		//TThostFtdcPriceType	LimitPrice;
		//typedef double TThostFtdcPriceType;
		napi_value LimitPrice;
		NAPI_CALL_RETURN_VOID(env_, napi_create_double(env_, pInputOrder->LimitPrice, &LimitPrice));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "LimitPrice", LimitPrice));

		///数量
		//TThostFtdcVolumeType	VolumeTotalOriginal;
		//typedef int TThostFtdcVolumeType;
		napi_value VolumeTotalOriginal;
		NAPI_CALL_RETURN_VOID(env_, napi_create_int32(env_, pInputOrder->VolumeTotalOriginal, &VolumeTotalOriginal));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "VolumeTotalOriginal", VolumeTotalOriginal));

		///有效期类型
		//TThostFtdcTimeConditionType	TimeCondition;
		//typedef char TThostFtdcTimeConditionType;
		napi_value TimeCondition;
		
		if (pInputOrder->TimeCondition != '\0') {
			NAPI_CALL_RETURN_VOID(env_, napi_create_string_utf8(env_, &pInputOrder->TimeCondition, 1, &TimeCondition));
		}
		else {
			NAPI_CALL_RETURN_VOID(env_, napi_create_string_utf8(env_, &pInputOrder->TimeCondition, 0, &TimeCondition));
		}
		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "TimeCondition", TimeCondition));

		///GTD日期
		//TThostFtdcDateType	GTDDate;
		//typedef char TThostFtdcDateType[9];
		napi_value GTDDate;
		NAPI_CALL_RETURN_VOID(env_, napi_create_string_utf8(env_, pInputOrder->GTDDate,
			NAPI_AUTO_LENGTH, &GTDDate));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "GTDDate", GTDDate));
		
		///成交量类型
		//TThostFtdcVolumeConditionType	VolumeCondition;
		//typedef char TThostFtdcVolumeConditionType;
		napi_value VolumeCondition;
		if (pInputOrder->VolumeCondition != '\0') {
			NAPI_CALL_RETURN_VOID(env_, napi_create_string_utf8(env_, &pInputOrder->VolumeCondition, 1, &VolumeCondition));
		}
		else {
			NAPI_CALL_RETURN_VOID(env_, napi_create_string_utf8(env_, &pInputOrder->VolumeCondition, 0, &VolumeCondition));
		}

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "VolumeCondition", VolumeCondition));

		///最小成交量
		//TThostFtdcVolumeType	MinVolume;
		//typedef int TThostFtdcVolumeType;
		napi_value MinVolume;
		NAPI_CALL_RETURN_VOID(env_, napi_create_int32(env_, pInputOrder->MinVolume, &MinVolume));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "MinVolume", MinVolume));

		///触发条件
		//TThostFtdcContingentConditionType	ContingentCondition;
		//typedef char TThostFtdcContingentConditionType;
		napi_value ContingentCondition;
		if (pInputOrder->ContingentCondition != '\0') {
			NAPI_CALL_RETURN_VOID(env_, napi_create_string_utf8(env_, &pInputOrder->ContingentCondition, 1, &ContingentCondition));
		}
		else {
			NAPI_CALL_RETURN_VOID(env_, napi_create_string_utf8(env_, &pInputOrder->ContingentCondition, 0, &ContingentCondition));
		}
		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "ContingentCondition", ContingentCondition));

		///止损价
		//TThostFtdcPriceType	StopPrice;
		//typedef double TThostFtdcPriceType;
		napi_value StopPrice;
		NAPI_CALL_RETURN_VOID(env_, napi_create_double(env_, pInputOrder->StopPrice, &LimitPrice));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "StopPrice", StopPrice));

		///强平原因
		//TThostFtdcForceCloseReasonType	ForceCloseReason;
		//typedef char TThostFtdcForceCloseReasonType;
		napi_value ForceCloseReason;
		if (pInputOrder->ForceCloseReason != '\0') {
			NAPI_CALL_RETURN_VOID(env_, napi_create_string_utf8(env_, &pInputOrder->ForceCloseReason, 1, &ForceCloseReason));
		}
		else {
			NAPI_CALL_RETURN_VOID(env_, napi_create_string_utf8(env_, &pInputOrder->ForceCloseReason, 0, &ForceCloseReason));
		}
		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "ForceCloseReason", ForceCloseReason));

		///自动挂起标志
		//TThostFtdcBoolType	IsAutoSuspend;
		//typedef int TThostFtdcBoolType;
		napi_value IsAutoSuspend;
		NAPI_CALL_RETURN_VOID(env_, napi_create_int32(env_, pInputOrder->IsAutoSuspend, &IsAutoSuspend));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "IsAutoSuspend", IsAutoSuspend));
		
		
		///业务单元
		//TThostFtdcBusinessUnitType	BusinessUnit;
		//typedef char TThostFtdcBusinessUnitType[21];
		napi_value BusinessUnit;
		NAPI_CALL_RETURN_VOID(env_, napi_create_string_utf8(env_, pInputOrder->BusinessUnit,
			NAPI_AUTO_LENGTH, &BusinessUnit));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "BusinessUnit", BusinessUnit));


		///请求编号
		//TThostFtdcRequestIDType	RequestID;
		//typedef int TThostFtdcRequestIDType;
		napi_value RequestID;
		NAPI_CALL_RETURN_VOID(env_, napi_create_int32(env_, pInputOrder->RequestID, &RequestID));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "RequestID", RequestID));

		///用户强评标志
		//TThostFtdcBoolType	UserForceClose;
		//typedef int TThostFtdcBoolType;
		napi_value UserForceClose;
		NAPI_CALL_RETURN_VOID(env_, napi_create_int32(env_, pInputOrder->UserForceClose, &UserForceClose));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "UserForceClose", UserForceClose));


		///互换单标志
		//TThostFtdcBoolType	IsSwapOrder;
		//typedef int TThostFtdcBoolType;
		napi_value IsSwapOrder;
		NAPI_CALL_RETURN_VOID(env_, napi_create_int32(env_, pInputOrder->IsSwapOrder, &IsSwapOrder));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "IsSwapOrder", IsSwapOrder));
		
		///交易所代码
		//TThostFtdcExchangeIDType	ExchangeID;
		//typedef char TThostFtdcExchangeIDType[9];
		napi_value ExchangeID;
		NAPI_CALL_RETURN_VOID(env_, napi_create_string_utf8(env_, pInputOrder->ExchangeID,
			NAPI_AUTO_LENGTH, &ExchangeID));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "ExchangeID", ExchangeID));

		///投资单元代码
		//TThostFtdcInvestUnitIDType	InvestUnitID;
		//typedef char TThostFtdcInvestUnitIDType[17];
		napi_value InvestUnitID;
		NAPI_CALL_RETURN_VOID(env_, napi_create_string_utf8(env_, pInputOrder->InvestUnitID,
			NAPI_AUTO_LENGTH, &InvestUnitID));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "InvestUnitID", InvestUnitID));
		
		///资金账号
		//TThostFtdcAccountIDType	AccountID;
		//typedef char TThostFtdcAccountIDType[13];
		napi_value AccountID;
		NAPI_CALL_RETURN_VOID(env_, napi_create_string_utf8(env_, pInputOrder->AccountID,
			NAPI_AUTO_LENGTH, &AccountID));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "AccountID", AccountID));
		
		///币种代码
		//TThostFtdcCurrencyIDType	CurrencyID;
		//typedef char TThostFtdcCurrencyIDType[4];
		napi_value CurrencyID;
		NAPI_CALL_RETURN_VOID(env_, napi_create_string_utf8(env_, pInputOrder->CurrencyID,
			NAPI_AUTO_LENGTH, &CurrencyID));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "CurrencyID", CurrencyID));

		///交易编码
		//TThostFtdcClientIDType	ClientID;
		//typedef char TThostFtdcClientIDType[11];
		napi_value ClientID;
		NAPI_CALL_RETURN_VOID(env_, napi_create_string_utf8(env_, pInputOrder->ClientID,
			NAPI_AUTO_LENGTH, &ClientID));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "ClientID", ClientID));


		///IP地址
		//TThostFtdcIPAddressType	IPAddress;
		//typedef char TThostFtdcIPAddressType[16];
		napi_value IPAddress;
		NAPI_CALL_RETURN_VOID(env_, napi_create_string_utf8(env_, pInputOrder->IPAddress,
			NAPI_AUTO_LENGTH, &IPAddress));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "IPAddress", IPAddress));

		///Mac地址
		//TThostFtdcMacAddressType	MacAddress;
		//typedef char TThostFtdcMacAddressType[21];
		napi_value MacAddress;
		NAPI_CALL_RETURN_VOID(env_, napi_create_string_utf8(env_, pInputOrder->MacAddress,
			NAPI_AUTO_LENGTH, &MacAddress));
		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "MacAddress", MacAddress));
	}
	else
	{
		NAPI_CALL_RETURN_VOID(env_, napi_get_undefined(env_, cbArgs));
	}

	CThostFtdcRspInfoField *pRspInfo = static_cast<CThostFtdcRspInfoField*>(data->rspInfo);
	
	pkg_rspinfo(pRspInfo, cbArgs + 1);

}

void CTPTraderClient::pkg_cb_rspinsertorder(OnEventCbRtnField* data, napi_value* cbArgs)
{
	CThostFtdcInputOrderField *pInputOrder = static_cast<CThostFtdcInputOrderField*>(data->rtnField);
	if (pInputOrder)
	{

		NAPI_CALL_RETURN_VOID(env_, napi_create_object(env_, cbArgs));

		///经纪公司代码
		//TThostFtdcBrokerIDType	BrokerID;
		//typedef char TThostFtdcBrokerIDType[11];
		napi_value BrokerID;
		NAPI_CALL_RETURN_VOID(env_, napi_create_string_utf8(env_, pInputOrder->BrokerID,
			NAPI_AUTO_LENGTH, &BrokerID));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "BrokerID", BrokerID));

		///投资者代码
		//TThostFtdcInvestorIDType	InvestorID;
		//typedef char TThostFtdcInvestorIDType[13];
		napi_value InvestorID;
		NAPI_CALL_RETURN_VOID(env_, napi_create_string_utf8(env_, pInputOrder->InvestorID,
			NAPI_AUTO_LENGTH, &InvestorID));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "InvestorID", InvestorID));

		///合约代码
		//TThostFtdcInstrumentIDType	InstrumentID;
		//typedef char TThostFtdcInstrumentIDType[31];
		napi_value InstrumentID;
		NAPI_CALL_RETURN_VOID(env_, napi_create_string_utf8(env_, pInputOrder->InstrumentID,
			NAPI_AUTO_LENGTH, &InstrumentID));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "InstrumentID", InstrumentID));

		///报单引用
		//TThostFtdcOrderRefType	OrderRef;
		//typedef char TThostFtdcOrderRefType[13];
		napi_value OrderRef;
		NAPI_CALL_RETURN_VOID(env_, napi_create_string_utf8(env_, pInputOrder->OrderRef,
			NAPI_AUTO_LENGTH, &OrderRef));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "OrderRef", OrderRef));

		///用户代码
		//TThostFtdcUserIDType	UserID;
		//typedef char TThostFtdcUserIDType[16];
		napi_value UserID;
		NAPI_CALL_RETURN_VOID(env_, napi_create_string_utf8(env_, pInputOrder->UserID,
			NAPI_AUTO_LENGTH, &UserID));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "UserID", UserID));

		///报单价格条件
		//TThostFtdcOrderPriceTypeType	OrderPriceType;
		//typedef char TThostFtdcOrderPriceTypeType;
		napi_value OrderPriceType;
		
		if (pInputOrder->OrderPriceType != '\0') {
			NAPI_CALL_RETURN_VOID(env_, napi_create_string_utf8(env_, &pInputOrder->OrderPriceType, 1, &OrderPriceType));
		}
		else {
			NAPI_CALL_RETURN_VOID(env_, napi_create_string_utf8(env_, &pInputOrder->OrderPriceType, 0, &OrderPriceType));
		}

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "OrderPriceType", OrderPriceType));



		///买卖方向
		//TThostFtdcDirectionType	Direction;
		//typedef char TThostFtdcDirectionType;
		napi_value Direction;
		
		if (pInputOrder->Direction != '\0') {
			NAPI_CALL_RETURN_VOID(env_, napi_create_string_utf8(env_, &pInputOrder->Direction, 1, &Direction));
		}
		else {
			NAPI_CALL_RETURN_VOID(env_, napi_create_string_utf8(env_, &pInputOrder->Direction, 0, &Direction));
		}

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "Direction", Direction));


		///组合开平标志
		//TThostFtdcCombOffsetFlagType	CombOffsetFlag;
		//typedef char TThostFtdcCombOffsetFlagType[5];
		napi_value CombOffsetFlag;
		NAPI_CALL_RETURN_VOID(env_, napi_create_string_utf8(env_, pInputOrder->CombOffsetFlag,
			NAPI_AUTO_LENGTH, &CombOffsetFlag));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "CombOffsetFlag", CombOffsetFlag));

		///组合投机套保标志
		//TThostFtdcCombHedgeFlagType	CombHedgeFlag;
		//typedef char TThostFtdcCombHedgeFlagType[5];
		napi_value CombHedgeFlag;
		NAPI_CALL_RETURN_VOID(env_, napi_create_string_utf8(env_, pInputOrder->CombHedgeFlag,
			NAPI_AUTO_LENGTH, &CombHedgeFlag));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "CombHedgeFlag", CombHedgeFlag));

		///价格
		//TThostFtdcPriceType	LimitPrice;
		//typedef double TThostFtdcPriceType;
		napi_value LimitPrice;
		NAPI_CALL_RETURN_VOID(env_, napi_create_double(env_, pInputOrder->LimitPrice, &LimitPrice));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "LimitPrice", LimitPrice));

		///数量
		//TThostFtdcVolumeType	VolumeTotalOriginal;
		//typedef int TThostFtdcVolumeType;
		napi_value VolumeTotalOriginal;
		NAPI_CALL_RETURN_VOID(env_, napi_create_int32(env_, pInputOrder->VolumeTotalOriginal, &VolumeTotalOriginal));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "VolumeTotalOriginal", VolumeTotalOriginal));

		///有效期类型
		//TThostFtdcTimeConditionType	TimeCondition;
		//typedef char TThostFtdcTimeConditionType;
		napi_value TimeCondition;
		
		if (pInputOrder->TimeCondition != '\0') {
			NAPI_CALL_RETURN_VOID(env_, napi_create_string_utf8(env_, &pInputOrder->TimeCondition, 1, &TimeCondition));
		}
		else {
			NAPI_CALL_RETURN_VOID(env_, napi_create_string_utf8(env_, &pInputOrder->TimeCondition, 0, &TimeCondition));
		}

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "TimeCondition", TimeCondition));

		///GTD日期
		//TThostFtdcDateType	GTDDate;
		//typedef char TThostFtdcDateType[9];
		napi_value GTDDate;
		NAPI_CALL_RETURN_VOID(env_, napi_create_string_utf8(env_, pInputOrder->GTDDate,
			NAPI_AUTO_LENGTH, &GTDDate));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "GTDDate", GTDDate));

		///成交量类型
		//TThostFtdcVolumeConditionType	VolumeCondition;
		//typedef char TThostFtdcVolumeConditionType;
		napi_value VolumeCondition;
		if (pInputOrder->VolumeCondition != '\0') {
			NAPI_CALL_RETURN_VOID(env_, napi_create_string_utf8(env_, &pInputOrder->VolumeCondition, 1, &VolumeCondition));
		}
		else {
			NAPI_CALL_RETURN_VOID(env_, napi_create_string_utf8(env_, &pInputOrder->VolumeCondition, 0, &VolumeCondition));
		}
		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "VolumeCondition", VolumeCondition));

		///最小成交量
		//TThostFtdcVolumeType	MinVolume;
		//typedef int TThostFtdcVolumeType;
		napi_value MinVolume;
		NAPI_CALL_RETURN_VOID(env_, napi_create_int32(env_, pInputOrder->MinVolume, &MinVolume));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "MinVolume", MinVolume));

		///触发条件
		//TThostFtdcContingentConditionType	ContingentCondition;
		//typedef char TThostFtdcContingentConditionType;
		napi_value ContingentCondition;
		if (pInputOrder->ContingentCondition != '\0') {
			NAPI_CALL_RETURN_VOID(env_, napi_create_string_utf8(env_, &pInputOrder->ContingentCondition, 1, &ContingentCondition));
		}
		else {
			NAPI_CALL_RETURN_VOID(env_, napi_create_string_utf8(env_, &pInputOrder->ContingentCondition, 0, &ContingentCondition));
		}

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "ContingentCondition", ContingentCondition));

		///止损价
		//TThostFtdcPriceType	StopPrice;
		//typedef double TThostFtdcPriceType;
		napi_value StopPrice;
		NAPI_CALL_RETURN_VOID(env_, napi_create_double(env_, pInputOrder->StopPrice, &StopPrice));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "StopPrice", StopPrice));

		///强平原因
		//TThostFtdcForceCloseReasonType	ForceCloseReason;
		//typedef char TThostFtdcForceCloseReasonType;
		napi_value ForceCloseReason;

		if (pInputOrder->ForceCloseReason != '\0') {
			NAPI_CALL_RETURN_VOID(env_, napi_create_string_utf8(env_, &pInputOrder->ForceCloseReason, 1, &ForceCloseReason));
		}
		else {
			NAPI_CALL_RETURN_VOID(env_, napi_create_string_utf8(env_, &pInputOrder->ForceCloseReason, 0, &ForceCloseReason));
		}

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "ForceCloseReason", ForceCloseReason));

		///自动挂起标志
		//TThostFtdcBoolType	IsAutoSuspend;
		//typedef int TThostFtdcBoolType;
		napi_value IsAutoSuspend;
		NAPI_CALL_RETURN_VOID(env_, napi_create_int32(env_, pInputOrder->IsAutoSuspend, &IsAutoSuspend));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "IsAutoSuspend", IsAutoSuspend));


		///业务单元
		//TThostFtdcBusinessUnitType	BusinessUnit;
		//typedef char TThostFtdcBusinessUnitType[21];
		napi_value BusinessUnit;
		NAPI_CALL_RETURN_VOID(env_, napi_create_string_utf8(env_, pInputOrder->BusinessUnit,
			NAPI_AUTO_LENGTH, &BusinessUnit));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "BusinessUnit", BusinessUnit));


		///请求编号
		//TThostFtdcRequestIDType	RequestID;
		//typedef int TThostFtdcRequestIDType;
		napi_value RequestID;
		NAPI_CALL_RETURN_VOID(env_, napi_create_int32(env_, pInputOrder->RequestID, &RequestID));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "RequestID", RequestID));

		///用户强评标志
		//TThostFtdcBoolType	UserForceClose;
		//typedef int TThostFtdcBoolType;
		napi_value UserForceClose;
		NAPI_CALL_RETURN_VOID(env_, napi_create_int32(env_, pInputOrder->UserForceClose, &UserForceClose));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "UserForceClose", UserForceClose));


		///互换单标志
		//TThostFtdcBoolType	IsSwapOrder;
		//typedef int TThostFtdcBoolType;
		napi_value IsSwapOrder;
		NAPI_CALL_RETURN_VOID(env_, napi_create_int32(env_, pInputOrder->IsSwapOrder, &IsSwapOrder));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "IsSwapOrder", IsSwapOrder));

		///交易所代码
		//TThostFtdcExchangeIDType	ExchangeID;
		//typedef char TThostFtdcExchangeIDType[9];
		napi_value ExchangeID;
		NAPI_CALL_RETURN_VOID(env_, napi_create_string_utf8(env_, pInputOrder->ExchangeID,
			NAPI_AUTO_LENGTH, &ExchangeID));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "ExchangeID", ExchangeID));

		///投资单元代码
		//TThostFtdcInvestUnitIDType	InvestUnitID;
		//typedef char TThostFtdcInvestUnitIDType[17];
		napi_value InvestUnitID;
		NAPI_CALL_RETURN_VOID(env_, napi_create_string_utf8(env_, pInputOrder->InvestUnitID,
			NAPI_AUTO_LENGTH, &InvestUnitID));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "InvestUnitID", InvestUnitID));

		///资金账号
		//TThostFtdcAccountIDType	AccountID;
		//typedef char TThostFtdcAccountIDType[13];
		napi_value AccountID;
		NAPI_CALL_RETURN_VOID(env_, napi_create_string_utf8(env_, pInputOrder->AccountID,
			NAPI_AUTO_LENGTH, &AccountID));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "AccountID", AccountID));

		///币种代码
		//TThostFtdcCurrencyIDType	CurrencyID;
		//typedef char TThostFtdcCurrencyIDType[4];
		napi_value CurrencyID;
		NAPI_CALL_RETURN_VOID(env_, napi_create_string_utf8(env_, pInputOrder->CurrencyID,
			NAPI_AUTO_LENGTH, &CurrencyID));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "CurrencyID", CurrencyID));

		///交易编码
		//TThostFtdcClientIDType	ClientID;
		//typedef char TThostFtdcClientIDType[11];
		napi_value ClientID;
		NAPI_CALL_RETURN_VOID(env_, napi_create_string_utf8(env_, pInputOrder->ClientID,
			NAPI_AUTO_LENGTH, &ClientID));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "ClientID", ClientID));


		///IP地址
		//TThostFtdcIPAddressType	IPAddress;
		//typedef char TThostFtdcIPAddressType[16];
		napi_value IPAddress;
		NAPI_CALL_RETURN_VOID(env_, napi_create_string_utf8(env_, pInputOrder->IPAddress,
			NAPI_AUTO_LENGTH, &IPAddress));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "IPAddress", IPAddress));

		///Mac地址
		//TThostFtdcMacAddressType	MacAddress;
		//typedef char TThostFtdcMacAddressType[21];
		napi_value MacAddress;
		NAPI_CALL_RETURN_VOID(env_, napi_create_string_utf8(env_, pInputOrder->MacAddress,
			NAPI_AUTO_LENGTH, &MacAddress));
		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "MacAddress", MacAddress));

	}
	else
	{
		NAPI_CALL_RETURN_VOID(env_, napi_get_undefined(env_, cbArgs));
	}

	CThostFtdcRspInfoField *pRspInfo = static_cast<CThostFtdcRspInfoField*>(data->rspInfo);
	pkg_rspinfo(pRspInfo, cbArgs + 1);

	NAPI_CALL_RETURN_VOID(env_, napi_create_int32(env_, data->nRequestID, (cbArgs + 2)));

	NAPI_CALL_RETURN_VOID(env_, napi_get_boolean(env_, data->bIsLast, (cbArgs + 3)));

}

void CTPTraderClient::pkg_cb_qryinvestorposition(OnEventCbRtnField* data, napi_value* cbArgs)
{
	CThostFtdcInvestorPositionField *pInvestorPosition = static_cast<CThostFtdcInvestorPositionField*>(data->rtnField);
	if (pInvestorPosition)
	{

		NAPI_CALL_RETURN_VOID(env_, napi_create_object(env_, cbArgs));

		///合约代码
		//TThostFtdcInstrumentIDType	InstrumentID;
		//typedef char TThostFtdcInstrumentIDType[31];
		napi_value InstrumentID;
		NAPI_CALL_RETURN_VOID(env_, napi_create_string_utf8(env_, pInvestorPosition->InstrumentID,
			NAPI_AUTO_LENGTH, &InstrumentID));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "InstrumentID", InstrumentID));


		///经纪公司代码
		//TThostFtdcBrokerIDType	BrokerID;
		//typedef char TThostFtdcBrokerIDType[11];
		napi_value BrokerID;
		NAPI_CALL_RETURN_VOID(env_, napi_create_string_utf8(env_, pInvestorPosition->BrokerID,
			NAPI_AUTO_LENGTH, &BrokerID));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "BrokerID", BrokerID));


		///投资者代码
		//TThostFtdcInvestorIDType	InvestorID;
		//typedef char TThostFtdcInvestorIDType[13];

		napi_value InvestorID;
		NAPI_CALL_RETURN_VOID(env_, napi_create_string_utf8(env_, pInvestorPosition->InvestorID,
			NAPI_AUTO_LENGTH, &InvestorID));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "InvestorID", InvestorID));

		///持仓多空方向
		//TThostFtdcPosiDirectionType	PosiDirection;
		//typedef char TThostFtdcPosiDirectionType;
		napi_value PosiDirection;
		if (pInvestorPosition->PosiDirection != '\0') {
			NAPI_CALL_RETURN_VOID(env_, napi_create_string_utf8(env_, &pInvestorPosition->PosiDirection, 1, &PosiDirection));
		}
		else {
			NAPI_CALL_RETURN_VOID(env_, napi_create_string_utf8(env_, &pInvestorPosition->PosiDirection, 0, &PosiDirection));
		}
		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "PosiDirection", PosiDirection));


		///投机套保标志
		//TThostFtdcHedgeFlagType	HedgeFlag;
		//typedef char TThostFtdcHedgeFlagType;
		napi_value HedgeFlag;

		if (pInvestorPosition->HedgeFlag != '\0') {
			NAPI_CALL_RETURN_VOID(env_, napi_create_string_utf8(env_, &pInvestorPosition->HedgeFlag, 1, &HedgeFlag));
		}
		else {
			NAPI_CALL_RETURN_VOID(env_, napi_create_string_utf8(env_, &pInvestorPosition->HedgeFlag, 0, &HedgeFlag));
		}

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "HedgeFlag", HedgeFlag));

		///持仓日期
		//TThostFtdcPositionDateType	PositionDate;
		//typedef char TThostFtdcPositionDateType;
		napi_value PositionDate;
		if (pInvestorPosition->PositionDate != '\0') {
			NAPI_CALL_RETURN_VOID(env_, napi_create_string_utf8(env_, &pInvestorPosition->PositionDate, 1, &PositionDate));
		}
		else {
			NAPI_CALL_RETURN_VOID(env_, napi_create_string_utf8(env_, &pInvestorPosition->PositionDate, 0, &PositionDate));
		}

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "PositionDate", PositionDate));

		///上日持仓
		//TThostFtdcVolumeType	YdPosition;
		//typedef int TThostFtdcVolumeType;
		napi_value YdPosition;
		NAPI_CALL_RETURN_VOID(env_, napi_create_int32(env_, pInvestorPosition->YdPosition, &YdPosition));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "YdPosition", YdPosition));

		///今日持仓
		//TThostFtdcVolumeType	Position;
		//typedef int TThostFtdcVolumeType;
		napi_value Position;
		NAPI_CALL_RETURN_VOID(env_, napi_create_int32(env_, pInvestorPosition->Position, &Position));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "Position", Position));

		///多头冻结
		//TThostFtdcVolumeType	LongFrozen;
		//typedef int TThostFtdcVolumeType;
		napi_value LongFrozen;
		NAPI_CALL_RETURN_VOID(env_, napi_create_int32(env_, pInvestorPosition->LongFrozen, &LongFrozen));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "LongFrozen", LongFrozen));

		///空头冻结
		//TThostFtdcVolumeType	ShortFrozen;
		//typedef int TThostFtdcVolumeType;
		napi_value ShortFrozen;
		NAPI_CALL_RETURN_VOID(env_, napi_create_int32(env_, pInvestorPosition->ShortFrozen, &ShortFrozen));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "ShortFrozen", ShortFrozen));

		///开仓冻结金额
		//TThostFtdcMoneyType	LongFrozenAmount;
		//typedef double TThostFtdcMoneyType;
		napi_value LongFrozenAmount;
		NAPI_CALL_RETURN_VOID(env_, napi_create_double(env_, pInvestorPosition->LongFrozenAmount, &LongFrozenAmount));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "LongFrozenAmount", LongFrozenAmount));

		///开仓冻结金额
		//TThostFtdcMoneyType	ShortFrozenAmount;
		//typedef double TThostFtdcMoneyType;
		napi_value ShortFrozenAmount;
		NAPI_CALL_RETURN_VOID(env_, napi_create_double(env_, pInvestorPosition->ShortFrozenAmount, &ShortFrozenAmount));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "ShortFrozenAmount", ShortFrozenAmount));

		///开仓量
		//TThostFtdcVolumeType	OpenVolume;
		//typedef int TThostFtdcVolumeType;
		napi_value OpenVolume;
		NAPI_CALL_RETURN_VOID(env_, napi_create_int32(env_, pInvestorPosition->OpenVolume, &OpenVolume));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "OpenVolume", OpenVolume));

		///平仓量
		//TThostFtdcVolumeType	CloseVolume;
		//typedef int TThostFtdcVolumeType;
		napi_value CloseVolume;
		NAPI_CALL_RETURN_VOID(env_, napi_create_int32(env_, pInvestorPosition->CloseVolume, &CloseVolume));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "CloseVolume", CloseVolume));

		///开仓金额
		//TThostFtdcMoneyType	OpenAmount;
		//typedef double TThostFtdcMoneyType;
		napi_value OpenAmount;
		NAPI_CALL_RETURN_VOID(env_, napi_create_double(env_, pInvestorPosition->OpenAmount, &OpenAmount));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "OpenAmount", OpenAmount));

		///平仓金额
		//TThostFtdcMoneyType	CloseAmount;
		//typedef double TThostFtdcMoneyType;
		napi_value CloseAmount;
		NAPI_CALL_RETURN_VOID(env_, napi_create_double(env_, pInvestorPosition->CloseAmount, &CloseAmount));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "CloseAmount", CloseAmount));

		///持仓成本
		//TThostFtdcMoneyType	PositionCost;
		//typedef double TThostFtdcMoneyType;
		napi_value PositionCost;
		NAPI_CALL_RETURN_VOID(env_, napi_create_double(env_, pInvestorPosition->PositionCost, &PositionCost));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "PositionCost", PositionCost));

		///上次占用的保证金
		//TThostFtdcMoneyType	PreMargin;
		//typedef double TThostFtdcMoneyType;
		napi_value PreMargin;
		NAPI_CALL_RETURN_VOID(env_, napi_create_double(env_, pInvestorPosition->PreMargin, &PreMargin));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "PreMargin", PreMargin));

		///占用的保证金
		//TThostFtdcMoneyType	UseMargin;
		//typedef double TThostFtdcMoneyType;
		napi_value UseMargin;
		NAPI_CALL_RETURN_VOID(env_, napi_create_double(env_, pInvestorPosition->UseMargin, &UseMargin));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "UseMargin", UseMargin));

		///冻结的保证金
		//TThostFtdcMoneyType	FrozenMargin;
		//typedef double TThostFtdcMoneyType;
		napi_value FrozenMargin;
		NAPI_CALL_RETURN_VOID(env_, napi_create_double(env_, pInvestorPosition->FrozenMargin, &FrozenMargin));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "FrozenMargin", FrozenMargin));

		///冻结的资金
		//TThostFtdcMoneyType	FrozenCash;
		//typedef double TThostFtdcMoneyType;
		napi_value FrozenCash;
		NAPI_CALL_RETURN_VOID(env_, napi_create_double(env_, pInvestorPosition->FrozenCash, &FrozenCash));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "FrozenCash", FrozenCash));
		
		///冻结的手续费
		//TThostFtdcMoneyType	FrozenCommission;
		//typedef double TThostFtdcMoneyType;
		napi_value FrozenCommission;
		NAPI_CALL_RETURN_VOID(env_, napi_create_double(env_, pInvestorPosition->FrozenCommission, &FrozenCommission));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "FrozenCommission", FrozenCommission));

		///资金差额
		//TThostFtdcMoneyType	CashIn;
		//typedef double TThostFtdcMoneyType;
		napi_value CashIn;
		NAPI_CALL_RETURN_VOID(env_, napi_create_double(env_, pInvestorPosition->CashIn, &CashIn));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "CashIn", CashIn));

		//手续费
		//TThostFtdcMoneyType	Commission;
		//typedef double TThostFtdcMoneyType;
		napi_value Commission;
		NAPI_CALL_RETURN_VOID(env_, napi_create_double(env_, pInvestorPosition->Commission, &Commission));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "Commission", Commission));

		///平仓盈亏
		//TThostFtdcMoneyType	CloseProfit;
		//typedef double TThostFtdcMoneyType;
		napi_value CloseProfit;
		NAPI_CALL_RETURN_VOID(env_, napi_create_double(env_, pInvestorPosition->CloseProfit, &CloseProfit));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "CloseProfit", CloseProfit));

		///持仓盈亏
		//TThostFtdcMoneyType	PositionProfit;
		//typedef double TThostFtdcMoneyType;
		napi_value PositionProfit;
		NAPI_CALL_RETURN_VOID(env_, napi_create_double(env_, pInvestorPosition->PositionProfit, &PositionProfit));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "PositionProfit", PositionProfit));

		///上次结算价
		//TThostFtdcPriceType	PreSettlementPrice;
		//typedef double TThostFtdcPriceType;
		napi_value PreSettlementPrice;
		NAPI_CALL_RETURN_VOID(env_, napi_create_double(env_, pInvestorPosition->PreSettlementPrice, &PreSettlementPrice));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "PreSettlementPrice", PreSettlementPrice));

		///本次结算价
		//TThostFtdcPriceType	SettlementPrice;
		//typedef double TThostFtdcPriceType;
		napi_value SettlementPrice;
		NAPI_CALL_RETURN_VOID(env_, napi_create_double(env_, pInvestorPosition->SettlementPrice, &SettlementPrice));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "SettlementPrice", SettlementPrice));


		///交易日
		//TThostFtdcDateType	TradingDay;
		//typedef char TThostFtdcDateType[9];
		napi_value TradingDay;
		NAPI_CALL_RETURN_VOID(env_, napi_create_string_utf8(env_, pInvestorPosition->TradingDay,
			NAPI_AUTO_LENGTH, &TradingDay));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "TradingDay", TradingDay));

		///结算编号
		//TThostFtdcSettlementIDType	SettlementID;
		//typedef int TThostFtdcSettlementIDType;
		napi_value SettlementID;
		NAPI_CALL_RETURN_VOID(env_, napi_create_int32(env_, pInvestorPosition->SettlementID, &SettlementID));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "SettlementID", SettlementID));

		///开仓成本
		//TThostFtdcMoneyType	OpenCost;
		//typedef double TThostFtdcMoneyType;
		napi_value OpenCost;
		NAPI_CALL_RETURN_VOID(env_, napi_create_double(env_, pInvestorPosition->OpenCost, &OpenCost));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "OpenCost", OpenCost));

		///交易所保证金
		//TThostFtdcMoneyType	ExchangeMargin;
		//typedef double TThostFtdcMoneyType;
		napi_value ExchangeMargin;
		NAPI_CALL_RETURN_VOID(env_, napi_create_double(env_, pInvestorPosition->ExchangeMargin, &ExchangeMargin));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "ExchangeMargin", ExchangeMargin));

		///组合成交形成的持仓
		//TThostFtdcVolumeType	CombPosition;
		//typedef int TThostFtdcVolumeType;
		napi_value CombPosition;
		NAPI_CALL_RETURN_VOID(env_, napi_create_int32(env_, pInvestorPosition->CombPosition, &CombPosition));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "CombPosition", CombPosition));

		///组合多头冻结
		//TThostFtdcVolumeType	CombLongFrozen;
		//typedef int TThostFtdcVolumeType;
		napi_value CombLongFrozen;
		NAPI_CALL_RETURN_VOID(env_, napi_create_int32(env_, pInvestorPosition->CombLongFrozen, &CombLongFrozen));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "CombLongFrozen", CombLongFrozen));

		///组合空头冻结
		//TThostFtdcVolumeType	CombShortFrozen;
		//typedef int TThostFtdcVolumeType;
		napi_value CombShortFrozen;
		NAPI_CALL_RETURN_VOID(env_, napi_create_int32(env_, pInvestorPosition->CombShortFrozen, &CombShortFrozen));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "CombShortFrozen", CombShortFrozen));

		///逐日盯市平仓盈亏
		//TThostFtdcMoneyType	CloseProfitByDate;
		//typedef double TThostFtdcMoneyType;
		napi_value CloseProfitByDate;
		NAPI_CALL_RETURN_VOID(env_, napi_create_double(env_, pInvestorPosition->CloseProfitByDate, &CloseProfitByDate));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "CloseProfitByDate", CloseProfitByDate));

		///逐笔对冲平仓盈亏
		//TThostFtdcMoneyType	CloseProfitByTrade;
		//typedef double TThostFtdcMoneyType;
		napi_value CloseProfitByTrade;
		NAPI_CALL_RETURN_VOID(env_, napi_create_double(env_, pInvestorPosition->CloseProfitByTrade, &CloseProfitByTrade));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "CloseProfitByTrade", CloseProfitByTrade));

		///今日持仓
		//TThostFtdcVolumeType	TodayPosition;
		//typedef int TThostFtdcVolumeType;
		napi_value TodayPosition;
		NAPI_CALL_RETURN_VOID(env_, napi_create_int32(env_, pInvestorPosition->TodayPosition, &TodayPosition));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "TodayPosition", TodayPosition));

		///保证金率
		//TThostFtdcRatioType	MarginRateByMoney;
		//typedef double TThostFtdcRatioType;
		napi_value MarginRateByMoney;
		NAPI_CALL_RETURN_VOID(env_, napi_create_double(env_, pInvestorPosition->MarginRateByMoney, &MarginRateByMoney));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "MarginRateByMoney", MarginRateByMoney));

		///保证金率(按手数)
		//TThostFtdcRatioType	MarginRateByVolume;
		//typedef double TThostFtdcRatioType;
		napi_value MarginRateByVolume;
		NAPI_CALL_RETURN_VOID(env_, napi_create_double(env_, pInvestorPosition->MarginRateByVolume, &MarginRateByVolume));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "MarginRateByVolume", MarginRateByVolume));

		///执行冻结
		//TThostFtdcVolumeType	StrikeFrozen;
		//typedef int TThostFtdcVolumeType;
		napi_value StrikeFrozen;
		NAPI_CALL_RETURN_VOID(env_, napi_create_int32(env_, pInvestorPosition->StrikeFrozen, &StrikeFrozen));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "StrikeFrozen", StrikeFrozen));

		///执行冻结金额
		//TThostFtdcMoneyType	StrikeFrozenAmount;
		//typedef double TThostFtdcMoneyType;
		napi_value StrikeFrozenAmount;
		NAPI_CALL_RETURN_VOID(env_, napi_create_double(env_, pInvestorPosition->StrikeFrozenAmount, &StrikeFrozenAmount));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "StrikeFrozenAmount", StrikeFrozenAmount));

		///放弃执行冻结
		//TThostFtdcVolumeType	AbandonFrozen;
		//typedef int TThostFtdcVolumeType;
		napi_value AbandonFrozen;
		NAPI_CALL_RETURN_VOID(env_, napi_create_int32(env_, pInvestorPosition->AbandonFrozen, &AbandonFrozen));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "AbandonFrozen", AbandonFrozen));

	}
	else
	{
		NAPI_CALL_RETURN_VOID(env_, napi_get_undefined(env_, cbArgs));
	}

	CThostFtdcRspInfoField *pRspInfo = static_cast<CThostFtdcRspInfoField*>(data->rspInfo);
	pkg_rspinfo(pRspInfo, cbArgs + 1);

	NAPI_CALL_RETURN_VOID(env_, napi_create_int32(env_, data->nRequestID, (cbArgs + 2)));

	NAPI_CALL_RETURN_VOID(env_, napi_get_boolean(env_, data->bIsLast, (cbArgs + 3)));


}

void CTPTraderClient::pkg_cb_qrytradingaccount(OnEventCbRtnField* data, napi_value* cbArgs)
{
	CThostFtdcTradingAccountField *pTradingAccount = static_cast<CThostFtdcTradingAccountField*>(data->rtnField);
	if (pTradingAccount)
	{
		NAPI_CALL_RETURN_VOID(env_, napi_create_object(env_, cbArgs));

		///经纪公司代码
		//TThostFtdcBrokerIDType	BrokerID;
		//typedef char TThostFtdcBrokerIDType[11];
		napi_value BrokerID;
		NAPI_CALL_RETURN_VOID(env_, napi_create_string_utf8(env_, pTradingAccount->BrokerID,
			NAPI_AUTO_LENGTH, &BrokerID));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "BrokerID", BrokerID));

		
		///投资者帐号
		//TThostFtdcAccountIDType	AccountID;
		//typedef char TThostFtdcAccountIDType[13];
		napi_value AccountID;
		NAPI_CALL_RETURN_VOID(env_, napi_create_string_utf8(env_, pTradingAccount->AccountID,
			NAPI_AUTO_LENGTH, &AccountID));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "AccountID", AccountID));
		
		///上次质押金额
		//TThostFtdcMoneyType	PreMortgage;
		//typedef double TThostFtdcMoneyType;
		napi_value PreMortgage;
		NAPI_CALL_RETURN_VOID(env_, napi_create_double(env_, pTradingAccount->PreMortgage, &PreMortgage));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "PreMortgage", PreMortgage));

		
		///上次信用额度
		//TThostFtdcMoneyType	PreCredit;
		//typedef double TThostFtdcMoneyType;
		napi_value PreCredit;
		NAPI_CALL_RETURN_VOID(env_, napi_create_double(env_, pTradingAccount->PreCredit, &PreCredit));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "PreCredit", PreCredit));

		
		///上次存款额
		//TThostFtdcMoneyType	PreDeposit;
		//typedef double TThostFtdcMoneyType;
		napi_value PreDeposit;
		NAPI_CALL_RETURN_VOID(env_, napi_create_double(env_, pTradingAccount->PreDeposit, &PreDeposit));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "PreDeposit", PreDeposit));

		
		///上次结算准备金
		//TThostFtdcMoneyType	PreBalance;
		//typedef double TThostFtdcMoneyType;
		napi_value PreBalance;
		NAPI_CALL_RETURN_VOID(env_, napi_create_double(env_, pTradingAccount->PreBalance, &PreBalance));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "PreBalance", PreBalance));

		
		///上次占用的保证金
		//TThostFtdcMoneyType	PreMargin;
		//typedef double TThostFtdcMoneyType;
		napi_value PreMargin;
		NAPI_CALL_RETURN_VOID(env_, napi_create_double(env_, pTradingAccount->PreMargin, &PreMargin));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "PreMargin", PreMargin));

		
		///利息基数
		//TThostFtdcMoneyType	InterestBase;
		//typedef double TThostFtdcMoneyType;
		napi_value InterestBase;
		NAPI_CALL_RETURN_VOID(env_, napi_create_double(env_, pTradingAccount->InterestBase, &InterestBase));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "InterestBase", InterestBase));

		
		///利息收入
		//TThostFtdcMoneyType	Interest;
		//typedef double TThostFtdcMoneyType;
		napi_value Interest;
		NAPI_CALL_RETURN_VOID(env_, napi_create_double(env_, pTradingAccount->Interest, &Interest));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "Interest", Interest));

		///入金金额
		//TThostFtdcMoneyType	Deposit;
		//typedef double TThostFtdcMoneyType;
		napi_value Deposit;
		NAPI_CALL_RETURN_VOID(env_, napi_create_double(env_, pTradingAccount->Deposit, &Deposit));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "Deposit", Deposit));

		
		///出金金额
		//TThostFtdcMoneyType	Withdraw;
		//typedef double TThostFtdcMoneyType;
		napi_value Withdraw;
		NAPI_CALL_RETURN_VOID(env_, napi_create_double(env_, pTradingAccount->Withdraw, &Withdraw));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "Withdraw", Withdraw));
		
		///冻结的保证金
		//TThostFtdcMoneyType	FrozenMargin;
		//typedef double TThostFtdcMoneyType;
		napi_value FrozenMargin;
		NAPI_CALL_RETURN_VOID(env_, napi_create_double(env_, pTradingAccount->FrozenMargin, &FrozenMargin));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "FrozenMargin", FrozenMargin));
		
		///冻结的资金
		//TThostFtdcMoneyType	FrozenCash;
		//typedef double TThostFtdcMoneyType;
		napi_value FrozenCash;
		NAPI_CALL_RETURN_VOID(env_, napi_create_double(env_, pTradingAccount->FrozenCash, &FrozenCash));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "FrozenCash", FrozenCash));

		///冻结的手续费
		//TThostFtdcMoneyType	FrozenCommission;
		//typedef double TThostFtdcMoneyType;
		napi_value FrozenCommission;
		NAPI_CALL_RETURN_VOID(env_, napi_create_double(env_, pTradingAccount->FrozenCommission, &FrozenCommission));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "FrozenCommission", FrozenCommission));

		
		///当前保证金总额
		//TThostFtdcMoneyType	CurrMargin;
		//typedef double TThostFtdcMoneyType;
		napi_value CurrMargin;
		NAPI_CALL_RETURN_VOID(env_, napi_create_double(env_, pTradingAccount->CurrMargin, &CurrMargin));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "CurrMargin", CurrMargin));

		///资金差额
		//TThostFtdcMoneyType	CashIn;
		//typedef double TThostFtdcMoneyType;
		napi_value CashIn;
		NAPI_CALL_RETURN_VOID(env_, napi_create_double(env_, pTradingAccount->CashIn, &CashIn));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "CashIn", CashIn));


		///手续费
		//TThostFtdcMoneyType	Commission;
		//typedef double TThostFtdcMoneyType;
		napi_value Commission;
		NAPI_CALL_RETURN_VOID(env_, napi_create_double(env_, pTradingAccount->Commission, &Commission));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "Commission", Commission));

		
		///平仓盈亏
		//TThostFtdcMoneyType	CloseProfit;
		//typedef double TThostFtdcMoneyType;
		napi_value CloseProfit;
		NAPI_CALL_RETURN_VOID(env_, napi_create_double(env_, pTradingAccount->CloseProfit, &CloseProfit));
		
		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "CloseProfit", CloseProfit));

		
		///持仓盈亏
		//TThostFtdcMoneyType	PositionProfit;
		//typedef double TThostFtdcMoneyType;
		napi_value PositionProfit;
		NAPI_CALL_RETURN_VOID(env_, napi_create_double(env_, pTradingAccount->PositionProfit, &PositionProfit));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "PositionProfit", PositionProfit));

		
		///期货结算准备金
		//TThostFtdcMoneyType	Balance;
		//typedef double TThostFtdcMoneyType;
		napi_value Balance;
		NAPI_CALL_RETURN_VOID(env_, napi_create_double(env_, pTradingAccount->Balance, &Balance));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "Balance", Balance));

		
		///可用资金
		//TThostFtdcMoneyType	Available;
		//typedef double TThostFtdcMoneyType;
		napi_value Available;
		NAPI_CALL_RETURN_VOID(env_, napi_create_double(env_, pTradingAccount->Available, &Available));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "Available", Available));

		
		///可取资金
		//TThostFtdcMoneyType	WithdrawQuota;
		//typedef double TThostFtdcMoneyType;
		napi_value WithdrawQuota;
		NAPI_CALL_RETURN_VOID(env_, napi_create_double(env_, pTradingAccount->WithdrawQuota, &WithdrawQuota));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "WithdrawQuota", WithdrawQuota));


		///基本准备金
		//TThostFtdcMoneyType	Reserve;
		//typedef double TThostFtdcMoneyType;
		napi_value Reserve;
		NAPI_CALL_RETURN_VOID(env_, napi_create_double(env_, pTradingAccount->Reserve, &Reserve));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "Reserve", Reserve));
		
		///交易日
		//TThostFtdcDateType	TradingDay;
		//typedef char TThostFtdcDateType[9];
		napi_value TradingDay;
		NAPI_CALL_RETURN_VOID(env_, napi_create_string_utf8(env_, pTradingAccount->TradingDay,
			NAPI_AUTO_LENGTH, &TradingDay));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "TradingDay", TradingDay));

		///结算编号
		//TThostFtdcSettlementIDType	SettlementID;
		//typedef int TThostFtdcSettlementIDType;
		napi_value SettlementID;
		NAPI_CALL_RETURN_VOID(env_, napi_create_int32(env_, pTradingAccount->SettlementID, &SettlementID));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "SettlementID", SettlementID));

		
		///信用额度
		//TThostFtdcMoneyType	Credit;
		//typedef double TThostFtdcMoneyType;
		napi_value Credit;
		NAPI_CALL_RETURN_VOID(env_, napi_create_double(env_, pTradingAccount->Credit, &Credit));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "Credit", Credit));

		
		///质押金额
		//TThostFtdcMoneyType	Mortgage;
		//typedef double TThostFtdcMoneyType;
		napi_value Mortgage;
		NAPI_CALL_RETURN_VOID(env_, napi_create_double(env_, pTradingAccount->Mortgage, &Mortgage));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "Mortgage", Mortgage));

		
		///交易所保证金
		//TThostFtdcMoneyType	ExchangeMargin;
		//typedef double TThostFtdcMoneyType;
		napi_value ExchangeMargin;
		NAPI_CALL_RETURN_VOID(env_, napi_create_double(env_, pTradingAccount->ExchangeMargin, &ExchangeMargin));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "ExchangeMargin", ExchangeMargin));

		
		///投资者交割保证金
		//TThostFtdcMoneyType	DeliveryMargin;
		//typedef double TThostFtdcMoneyType;
		napi_value DeliveryMargin;
		NAPI_CALL_RETURN_VOID(env_, napi_create_double(env_, pTradingAccount->DeliveryMargin, &DeliveryMargin));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "DeliveryMargin", DeliveryMargin));

		
		///交易所交割保证金
		//TThostFtdcMoneyType	ExchangeDeliveryMargin;
		//typedef double TThostFtdcMoneyType;
		napi_value ExchangeDeliveryMargin;
		NAPI_CALL_RETURN_VOID(env_, napi_create_double(env_, pTradingAccount->ExchangeDeliveryMargin, &ExchangeDeliveryMargin));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "ExchangeDeliveryMargin", ExchangeDeliveryMargin));

		
		///保底期货结算准备金
		//TThostFtdcMoneyType	ReserveBalance;
		//typedef double TThostFtdcMoneyType;
		napi_value ReserveBalance;
		NAPI_CALL_RETURN_VOID(env_, napi_create_double(env_, pTradingAccount->ReserveBalance, &ReserveBalance));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "ReserveBalance", ReserveBalance));


		///币种代码
		//TThostFtdcCurrencyIDType	CurrencyID;
		//typedef char TThostFtdcCurrencyIDType[4];
		napi_value CurrencyID;
		NAPI_CALL_RETURN_VOID(env_, napi_create_string_utf8(env_, pTradingAccount->CurrencyID,
			NAPI_AUTO_LENGTH, &CurrencyID));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "CurrencyID", CurrencyID));

		
		///上次货币质入金额
		//TThostFtdcMoneyType	PreFundMortgageIn;
		//typedef double TThostFtdcMoneyType;
		napi_value PreFundMortgageIn;
		NAPI_CALL_RETURN_VOID(env_, napi_create_double(env_, pTradingAccount->PreFundMortgageIn, &PreFundMortgageIn));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "PreFundMortgageIn", PreFundMortgageIn));


		
		///上次货币质出金额
		//TThostFtdcMoneyType	PreFundMortgageOut;
		//typedef double TThostFtdcMoneyType;
		napi_value PreFundMortgageOut;
		NAPI_CALL_RETURN_VOID(env_, napi_create_double(env_, pTradingAccount->PreFundMortgageOut, &PreFundMortgageOut));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "PreFundMortgageOut", PreFundMortgageOut));

		
		///货币质入金额
		//TThostFtdcMoneyType	FundMortgageIn;
		//typedef double TThostFtdcMoneyType;
		napi_value FundMortgageIn;
		NAPI_CALL_RETURN_VOID(env_, napi_create_double(env_, pTradingAccount->FundMortgageIn, &FundMortgageIn));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "FundMortgageIn", FundMortgageIn));

		
		///货币质出金额
		//TThostFtdcMoneyType	FundMortgageOut;
		//typedef double TThostFtdcMoneyType;
		napi_value FundMortgageOut;
		NAPI_CALL_RETURN_VOID(env_, napi_create_double(env_, pTradingAccount->FundMortgageOut, &FundMortgageOut));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "FundMortgageOut", FundMortgageOut));

		
		///货币质押余额
		//TThostFtdcMoneyType	FundMortgageAvailable;
		//typedef double TThostFtdcMoneyType;
		napi_value FundMortgageAvailable;
		NAPI_CALL_RETURN_VOID(env_, napi_create_double(env_, pTradingAccount->FundMortgageAvailable, &FundMortgageAvailable));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "FundMortgageAvailable", FundMortgageAvailable));

		
		///可质押货币金额
		//TThostFtdcMoneyType	MortgageableFund;
		//typedef double TThostFtdcMoneyType;
		napi_value MortgageableFund;
		NAPI_CALL_RETURN_VOID(env_, napi_create_double(env_, pTradingAccount->MortgageableFund, &MortgageableFund));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "MortgageableFund", MortgageableFund));

		
		///特殊产品占用保证金
		//TThostFtdcMoneyType	SpecProductMargin;
		//typedef double TThostFtdcMoneyType;
		napi_value SpecProductMargin;
		NAPI_CALL_RETURN_VOID(env_, napi_create_double(env_, pTradingAccount->SpecProductMargin, &SpecProductMargin));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "SpecProductMargin", SpecProductMargin));

		
		///特殊产品冻结保证金
		//TThostFtdcMoneyType	SpecProductFrozenMargin;
		//typedef double TThostFtdcMoneyType;
		napi_value SpecProductFrozenMargin;
		NAPI_CALL_RETURN_VOID(env_, napi_create_double(env_, pTradingAccount->SpecProductFrozenMargin, &SpecProductFrozenMargin));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "SpecProductFrozenMargin", SpecProductFrozenMargin));

		
		///特殊产品手续费
		//TThostFtdcMoneyType	SpecProductCommission;
		//typedef double TThostFtdcMoneyType;
		napi_value SpecProductCommission;
		NAPI_CALL_RETURN_VOID(env_, napi_create_double(env_, pTradingAccount->SpecProductCommission, &SpecProductCommission));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "SpecProductCommission", SpecProductCommission));

		
		///特殊产品冻结手续费
		//TThostFtdcMoneyType	SpecProductFrozenCommission;
		//typedef double TThostFtdcMoneyType;
		napi_value SpecProductFrozenCommission;
		NAPI_CALL_RETURN_VOID(env_, napi_create_double(env_, pTradingAccount->SpecProductFrozenCommission, &SpecProductFrozenCommission));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "SpecProductFrozenCommission", SpecProductFrozenCommission));

		
		///特殊产品持仓盈亏
		//TThostFtdcMoneyType	SpecProductPositionProfit;
		//typedef double TThostFtdcMoneyType;
		napi_value SpecProductPositionProfit;
		NAPI_CALL_RETURN_VOID(env_, napi_create_double(env_, pTradingAccount->SpecProductPositionProfit, &SpecProductPositionProfit));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "SpecProductPositionProfit", SpecProductPositionProfit));

		
		///特殊产品平仓盈亏
		//TThostFtdcMoneyType	SpecProductCloseProfit;
		//typedef double TThostFtdcMoneyType;
		napi_value SpecProductCloseProfit;
		NAPI_CALL_RETURN_VOID(env_, napi_create_double(env_, pTradingAccount->SpecProductCloseProfit, &SpecProductCloseProfit));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "SpecProductCloseProfit", SpecProductCloseProfit));
		
		///根据持仓盈亏算法计算的特殊产品持仓盈亏
		//TThostFtdcMoneyType	SpecProductPositionProfitByAlg;
		//typedef double TThostFtdcMoneyType;
		napi_value SpecProductPositionProfitByAlg;
		NAPI_CALL_RETURN_VOID(env_, napi_create_double(env_, pTradingAccount->SpecProductPositionProfitByAlg, &SpecProductPositionProfitByAlg));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "SpecProductPositionProfitByAlg", SpecProductPositionProfitByAlg));

		
		///特殊产品交易所保证金
		//TThostFtdcMoneyType	SpecProductExchangeMargin;
		//typedef double TThostFtdcMoneyType;
		napi_value SpecProductExchangeMargin;
		NAPI_CALL_RETURN_VOID(env_, napi_create_double(env_, pTradingAccount->SpecProductExchangeMargin, &SpecProductExchangeMargin));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "SpecProductExchangeMargin", SpecProductExchangeMargin));

	}
	else
	{
		NAPI_CALL_RETURN_VOID(env_, napi_get_undefined(env_, cbArgs));
	}

	CThostFtdcRspInfoField *pRspInfo = static_cast<CThostFtdcRspInfoField*>(data->rspInfo);
	pkg_rspinfo(pRspInfo, cbArgs + 1);

	NAPI_CALL_RETURN_VOID(env_, napi_create_int32(env_, data->nRequestID, (cbArgs + 2)));

	NAPI_CALL_RETURN_VOID(env_, napi_get_boolean(env_, data->bIsLast, (cbArgs + 3)));

}

void CTPTraderClient::pkg_cb_qryinstrument(OnEventCbRtnField* data, napi_value* cbArgs)
{
	CThostFtdcInstrumentField *pInstrument = static_cast<CThostFtdcInstrumentField*>(data->rtnField);
	if (pInstrument)
	{
		NAPI_CALL_RETURN_VOID(env_, napi_create_object(env_, cbArgs));

		///合约代码
		//TThostFtdcInstrumentIDType	InstrumentID;
		//typedef char TThostFtdcInstrumentIDType[31];
		napi_value InstrumentID;
		NAPI_CALL_RETURN_VOID(env_, napi_create_string_utf8(env_, pInstrument->InstrumentID,
			NAPI_AUTO_LENGTH, &InstrumentID));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "InstrumentID", InstrumentID));

		///交易所代码
		//TThostFtdcExchangeIDType	ExchangeID;
		//typedef char TThostFtdcExchangeIDType[9];
		napi_value ExchangeID;
		NAPI_CALL_RETURN_VOID(env_, napi_create_string_utf8(env_, pInstrument->ExchangeID,
			NAPI_AUTO_LENGTH, &ExchangeID));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "ExchangeID", ExchangeID));
		
		///合约名称
		//TThostFtdcInstrumentNameType	InstrumentName;
		//typedef char TThostFtdcInstrumentNameType[21];
		napi_value InstrumentName;
		NAPI_CALL_RETURN_VOID(env_, napi_create_string_utf8(env_, pInstrument->InstrumentName,
			NAPI_AUTO_LENGTH, &InstrumentName));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "InstrumentName", InstrumentName));

		
		///合约在交易所的代码
		//TThostFtdcExchangeInstIDType	ExchangeInstID;
		//typedef char TThostFtdcExchangeInstIDType[31];
		napi_value ExchangeInstID;
		NAPI_CALL_RETURN_VOID(env_, napi_create_string_utf8(env_, pInstrument->ExchangeInstID,
			NAPI_AUTO_LENGTH, &ExchangeInstID));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "ExchangeInstID", ExchangeInstID));

		///产品代码
		//TThostFtdcInstrumentIDType	ProductID;
		//typedef char TThostFtdcInstrumentIDType[31];
		napi_value ProductID;
		NAPI_CALL_RETURN_VOID(env_, napi_create_string_utf8(env_, pInstrument->ProductID,
			NAPI_AUTO_LENGTH, &ProductID));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "ProductID", ProductID));

		
		///产品类型
		//TThostFtdcProductClassType	ProductClass;
		//typedef char TThostFtdcProductClassType;
		napi_value ProductClass;
		if (pInstrument->ProductClass != '\0') {
			NAPI_CALL_RETURN_VOID(env_, napi_create_string_utf8(env_, &pInstrument->ProductClass, 1, &ProductClass));
		}
		else {
			NAPI_CALL_RETURN_VOID(env_, napi_create_string_utf8(env_, &pInstrument->ProductClass, 0, &ProductClass));
		}
		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "ProductClass", ProductClass));

		
		///交割年份
		//TThostFtdcYearType	DeliveryYear;
		//typedef int TThostFtdcYearType;
		napi_value DeliveryYear;
		NAPI_CALL_RETURN_VOID(env_, napi_create_int32(env_, pInstrument->DeliveryYear, &DeliveryYear));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "DeliveryYear", DeliveryYear));

		
		///交割月
		//TThostFtdcMonthType	DeliveryMonth;
		//typedef int TThostFtdcMonthType;
		napi_value DeliveryMonth;
		NAPI_CALL_RETURN_VOID(env_, napi_create_int32(env_, pInstrument->DeliveryMonth, &DeliveryMonth));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "DeliveryMonth", DeliveryMonth));

		
		///市价单最大下单量
		//TThostFtdcVolumeType	MaxMarketOrderVolume;
		//typedef int TThostFtdcMonthType;
		napi_value MaxMarketOrderVolume;
		NAPI_CALL_RETURN_VOID(env_, napi_create_int32(env_, pInstrument->MaxMarketOrderVolume, &MaxMarketOrderVolume));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "MaxMarketOrderVolume", MaxMarketOrderVolume));

		
		///市价单最小下单量
		//TThostFtdcVolumeType	MinMarketOrderVolume;
		//typedef int TThostFtdcVolumeType;
		
		napi_value MinMarketOrderVolume;
		NAPI_CALL_RETURN_VOID(env_, napi_create_int32(env_, pInstrument->MinMarketOrderVolume, &MinMarketOrderVolume));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "MinMarketOrderVolume", MinMarketOrderVolume));
		
		///限价单最大下单量
		//TThostFtdcVolumeType	MaxLimitOrderVolume;
		//typedef int TThostFtdcVolumeType;
		napi_value MaxLimitOrderVolume;
		NAPI_CALL_RETURN_VOID(env_, napi_create_int32(env_, pInstrument->MaxLimitOrderVolume, &MaxLimitOrderVolume));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "MaxLimitOrderVolume", MaxLimitOrderVolume));

		
		///限价单最小下单量
		//TThostFtdcVolumeType	MinLimitOrderVolume;
		//typedef int TThostFtdcVolumeType;
		napi_value MinLimitOrderVolume;
		NAPI_CALL_RETURN_VOID(env_, napi_create_int32(env_, pInstrument->MinLimitOrderVolume, &MinLimitOrderVolume));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "MinLimitOrderVolume", MinLimitOrderVolume));

		
		///合约数量乘数
		//TThostFtdcVolumeMultipleType	VolumeMultiple;
		//typedef int TThostFtdcVolumeMultipleType;
		napi_value VolumeMultiple;
		NAPI_CALL_RETURN_VOID(env_, napi_create_int32(env_, pInstrument->VolumeMultiple, &VolumeMultiple));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "VolumeMultiple", VolumeMultiple));

		
		///最小变动价位
		//TThostFtdcPriceType	PriceTick;
		//typedef double TThostFtdcPriceType;
		napi_value PriceTick;
		NAPI_CALL_RETURN_VOID(env_, napi_create_double(env_, pInstrument->PriceTick, &PriceTick));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "PriceTick", PriceTick));

		
		///创建日
		//TThostFtdcDateType	CreateDate;
		//typedef char TThostFtdcDateType[9];
		napi_value CreateDate;
		NAPI_CALL_RETURN_VOID(env_, napi_create_string_utf8(env_, pInstrument->CreateDate,
			NAPI_AUTO_LENGTH, &CreateDate));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "CreateDate", CreateDate));

		
		///上市日
		//TThostFtdcDateType	OpenDate;
		//typedef char TThostFtdcDateType[9];
		napi_value OpenDate;
		NAPI_CALL_RETURN_VOID(env_, napi_create_string_utf8(env_, pInstrument->OpenDate,
			NAPI_AUTO_LENGTH, &OpenDate));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "OpenDate", OpenDate));

		
		///到期日
		//TThostFtdcDateType	ExpireDate;
		//typedef char TThostFtdcDateType[9];
		napi_value ExpireDate;
		NAPI_CALL_RETURN_VOID(env_, napi_create_string_utf8(env_, pInstrument->ExpireDate,
			NAPI_AUTO_LENGTH, &ExpireDate));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "ExpireDate", ExpireDate));

		
		///开始交割日
		//TThostFtdcDateType	StartDelivDate;
		//typedef char TThostFtdcDateType[9];
		napi_value StartDelivDate;
		NAPI_CALL_RETURN_VOID(env_, napi_create_string_utf8(env_, pInstrument->StartDelivDate,
			NAPI_AUTO_LENGTH, &StartDelivDate));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "StartDelivDate", StartDelivDate));

		
		///结束交割日
		//TThostFtdcDateType	EndDelivDate;
		//typedef char TThostFtdcDateType[9];
		napi_value EndDelivDate;
		NAPI_CALL_RETURN_VOID(env_, napi_create_string_utf8(env_, pInstrument->EndDelivDate,
			NAPI_AUTO_LENGTH, &EndDelivDate));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "EndDelivDate", EndDelivDate));

		
		///合约生命周期状态
		//TThostFtdcInstLifePhaseType	InstLifePhase;
		//typedef char TThostFtdcInstLifePhaseType;
		napi_value InstLifePhase;

		if (pInstrument->InstLifePhase != '\0') {
			NAPI_CALL_RETURN_VOID(env_, napi_create_string_utf8(env_, &pInstrument->InstLifePhase, 1, &InstLifePhase));
		}
		else {
			NAPI_CALL_RETURN_VOID(env_, napi_create_string_utf8(env_, &pInstrument->InstLifePhase, 0, &InstLifePhase));
		}

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "InstLifePhase", InstLifePhase));

		
		///当前是否交易
		//TThostFtdcBoolType	IsTrading;
		//typedef int TThostFtdcBoolType;
		napi_value IsTrading;
		NAPI_CALL_RETURN_VOID(env_, napi_create_int32(env_, pInstrument->IsTrading, &IsTrading));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "IsTrading", IsTrading));

		
		///持仓类型
		//TThostFtdcPositionTypeType	PositionType;
		//typedef char TThostFtdcPositionTypeType;
		napi_value PositionType;

		if (pInstrument->PositionType != '\0') {
			NAPI_CALL_RETURN_VOID(env_, napi_create_string_utf8(env_, &pInstrument->PositionType, 1, &PositionType));
		}
		else {
			NAPI_CALL_RETURN_VOID(env_, napi_create_string_utf8(env_, &pInstrument->PositionType, 0, &PositionType));
		}

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "PositionType", PositionType));

		
		///持仓日期类型
		//TThostFtdcPositionDateTypeType	PositionDateType;
		//typedef char TThostFtdcPositionDateTypeType;
		napi_value PositionDateType;
		if (pInstrument->PositionDateType != '\0') {
			NAPI_CALL_RETURN_VOID(env_, napi_create_string_utf8(env_, &pInstrument->PositionDateType, 1, &PositionDateType));
		}
		else {
			NAPI_CALL_RETURN_VOID(env_, napi_create_string_utf8(env_, &pInstrument->PositionDateType, 0, &PositionDateType));
		}

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "PositionDateType", PositionDateType));

		
		///多头保证金率
		//TThostFtdcRatioType	LongMarginRatio;
		//typedef double TThostFtdcRatioType;
		napi_value LongMarginRatio;
		NAPI_CALL_RETURN_VOID(env_, napi_create_double(env_, pInstrument->LongMarginRatio, &LongMarginRatio));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "LongMarginRatio", LongMarginRatio));

		///空头保证金率
		//TThostFtdcRatioType	ShortMarginRatio;
		napi_value ShortMarginRatio;
		NAPI_CALL_RETURN_VOID(env_, napi_create_double(env_, pInstrument->ShortMarginRatio, &ShortMarginRatio));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "ShortMarginRatio", ShortMarginRatio));

		
		
		///是否使用大额单边保证金算法
		//TThostFtdcMaxMarginSideAlgorithmType	MaxMarginSideAlgorithm;
		//typedef char TThostFtdcMaxMarginSideAlgorithmType;
		napi_value MaxMarginSideAlgorithm;

		if (pInstrument->MaxMarginSideAlgorithm != '\0') {
			NAPI_CALL_RETURN_VOID(env_, napi_create_string_utf8(env_, &pInstrument->MaxMarginSideAlgorithm, 1, &MaxMarginSideAlgorithm));
		}
		else {
			NAPI_CALL_RETURN_VOID(env_, napi_create_string_utf8(env_, &pInstrument->MaxMarginSideAlgorithm, 0, &MaxMarginSideAlgorithm));
		}

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "MaxMarginSideAlgorithm", MaxMarginSideAlgorithm));

		
		///基础商品代码
		//TThostFtdcInstrumentIDType	UnderlyingInstrID;
		//typedef char TThostFtdcInstrumentIDType[31];
		napi_value UnderlyingInstrID;
		NAPI_CALL_RETURN_VOID(env_, napi_create_string_utf8(env_, pInstrument->UnderlyingInstrID,
			NAPI_AUTO_LENGTH, &UnderlyingInstrID));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "UnderlyingInstrID", UnderlyingInstrID));

		///执行价
		//TThostFtdcPriceType	StrikePrice;
		//typedef double TThostFtdcRatioType;
		napi_value StrikePrice;
		NAPI_CALL_RETURN_VOID(env_, napi_create_double(env_, pInstrument->StrikePrice, &StrikePrice));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "StrikePrice", StrikePrice));

		
		///期权类型
		//TThostFtdcOptionsTypeType	OptionsType;
		//typedef char TThostFtdcOptionsTypeType;
		napi_value OptionsType;

		if (pInstrument->OptionsType != '\0') {
			NAPI_CALL_RETURN_VOID(env_, napi_create_string_utf8(env_, &pInstrument->OptionsType, 1, &OptionsType));
		}
		else {
			NAPI_CALL_RETURN_VOID(env_, napi_create_string_utf8(env_, &pInstrument->OptionsType, 0, &OptionsType));
		}

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "OptionsType", OptionsType));

		
		///合约基础商品乘数
		//TThostFtdcUnderlyingMultipleType	UnderlyingMultiple;
		//typedef double TThostFtdcUnderlyingMultipleType;
		napi_value UnderlyingMultiple;
		NAPI_CALL_RETURN_VOID(env_, napi_create_double(env_, pInstrument->UnderlyingMultiple, &UnderlyingMultiple));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "UnderlyingMultiple", UnderlyingMultiple));

		
		///组合类型
		//TThostFtdcCombinationTypeType	CombinationType;
		//typedef char TThostFtdcCombinationTypeType;
		napi_value CombinationType;

		if (pInstrument->CombinationType != '\0') {
			NAPI_CALL_RETURN_VOID(env_, napi_create_string_utf8(env_, &pInstrument->CombinationType, 1, &CombinationType));
		}
		else {
			NAPI_CALL_RETURN_VOID(env_, napi_create_string_utf8(env_, &pInstrument->CombinationType, 0, &CombinationType));
		}

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "CombinationType", CombinationType));

	}
	else
	{
		NAPI_CALL_RETURN_VOID(env_, napi_get_undefined(env_, cbArgs));
	}

	CThostFtdcRspInfoField *pRspInfo = static_cast<CThostFtdcRspInfoField*>(data->rspInfo);
	pkg_rspinfo(pRspInfo, cbArgs + 1);

	NAPI_CALL_RETURN_VOID(env_, napi_create_int32(env_, data->nRequestID, (cbArgs + 2)));

	NAPI_CALL_RETURN_VOID(env_, napi_get_boolean(env_, data->bIsLast, (cbArgs + 3)));

}


void CTPTraderClient::pkg_cb_RspQryInstrumentCommissionRate(OnEventCbRtnField* data, napi_value* cbArgs)
{
	CThostFtdcInstrumentCommissionRateField *pInstrumentCommissionRate = static_cast<CThostFtdcInstrumentCommissionRateField*>(data->rtnField);

	if (pInstrumentCommissionRate)
	{

		NAPI_CALL_RETURN_VOID(env_, napi_create_object(env_, cbArgs));


		///合约代码
		//TThostFtdcInstrumentIDType	InstrumentID;
		//typedef char TThostFtdcInstrumentIDType[31];
		napi_value InstrumentID;
		NAPI_CALL_RETURN_VOID(env_, napi_create_string_utf8(env_, pInstrumentCommissionRate->InstrumentID,
			NAPI_AUTO_LENGTH, &InstrumentID));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "InstrumentID", InstrumentID));

		
		///投资者范围
		//TThostFtdcInvestorRangeType	InvestorRange;
		//typedef char TThostFtdcInvestorRangeType;
		napi_value InvestorRange;

		if (pInstrumentCommissionRate->InvestorRange != '\0') {
			NAPI_CALL_RETURN_VOID(env_, napi_create_string_utf8(env_, &pInstrumentCommissionRate->InvestorRange, 1, &InvestorRange));
		}
		else {
			NAPI_CALL_RETURN_VOID(env_, napi_create_string_utf8(env_, &pInstrumentCommissionRate->InvestorRange, 0, &InvestorRange));
		}

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "InvestorRange", InvestorRange));

		
		///经纪公司代码
		//TThostFtdcBrokerIDType	BrokerID;
		//typedef char TThostFtdcBrokerIDType[11];
		napi_value BrokerID;
		NAPI_CALL_RETURN_VOID(env_, napi_create_string_utf8(env_, pInstrumentCommissionRate->BrokerID,
			NAPI_AUTO_LENGTH, &BrokerID));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "BrokerID", BrokerID));

		
		///投资者代码
		//TThostFtdcInvestorIDType	InvestorID;
		//typedef char TThostFtdcInvestorIDType[13];
		napi_value InvestorID;
		NAPI_CALL_RETURN_VOID(env_, napi_create_string_utf8(env_, pInstrumentCommissionRate->InvestorID,
			NAPI_AUTO_LENGTH, &InvestorID));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "InvestorID", InvestorID));

		
		///开仓手续费率
		//TThostFtdcRatioType	OpenRatioByMoney;
		//typedef double TThostFtdcRatioType;
		napi_value OpenRatioByMoney;
		NAPI_CALL_RETURN_VOID(env_, napi_create_double(env_, pInstrumentCommissionRate->OpenRatioByMoney, &OpenRatioByMoney));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "OpenRatioByMoney", OpenRatioByMoney));

		
		
		///开仓手续费
		//TThostFtdcRatioType	OpenRatioByVolume;
		//typedef double TThostFtdcRatioType;
		napi_value OpenRatioByVolume;
		NAPI_CALL_RETURN_VOID(env_, napi_create_double(env_, pInstrumentCommissionRate->OpenRatioByVolume, &OpenRatioByVolume));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "OpenRatioByVolume", OpenRatioByVolume));

		
		///平仓手续费率
		//TThostFtdcRatioType	CloseRatioByMoney;
		//typedef double TThostFtdcRatioType;
		napi_value CloseRatioByMoney;
		NAPI_CALL_RETURN_VOID(env_, napi_create_double(env_, pInstrumentCommissionRate->CloseRatioByMoney, &CloseRatioByMoney));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "CloseRatioByMoney", CloseRatioByMoney));

		
		///平仓手续费
		//TThostFtdcRatioType	CloseRatioByVolume;
		//typedef double TThostFtdcRatioType;
		napi_value CloseRatioByVolume;
		NAPI_CALL_RETURN_VOID(env_, napi_create_double(env_, pInstrumentCommissionRate->CloseRatioByVolume, &CloseRatioByVolume));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "CloseRatioByVolume", CloseRatioByVolume));

		
		///平今手续费率
		//TThostFtdcRatioType	CloseTodayRatioByMoney;
		//typedef double TThostFtdcRatioType;
		napi_value CloseTodayRatioByMoney;
		NAPI_CALL_RETURN_VOID(env_, napi_create_double(env_, pInstrumentCommissionRate->CloseTodayRatioByMoney, &CloseTodayRatioByMoney));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "CloseTodayRatioByMoney", CloseTodayRatioByMoney));

		
		///平今手续费
		//TThostFtdcRatioType	CloseTodayRatioByVolume;
		//typedef double TThostFtdcRatioType;
		napi_value CloseTodayRatioByVolume;
		NAPI_CALL_RETURN_VOID(env_, napi_create_double(env_, pInstrumentCommissionRate->CloseTodayRatioByVolume, &CloseTodayRatioByVolume));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "CloseTodayRatioByVolume", CloseTodayRatioByVolume));

	}
	else
	{
		NAPI_CALL_RETURN_VOID(env_, napi_get_undefined(env_, cbArgs));
	}

	CThostFtdcRspInfoField *pRspInfo = static_cast<CThostFtdcRspInfoField*>(data->rspInfo);
	pkg_rspinfo(pRspInfo, cbArgs + 1);

	NAPI_CALL_RETURN_VOID(env_, napi_create_int32(env_, data->nRequestID, (cbArgs + 2)));

	NAPI_CALL_RETURN_VOID(env_, napi_get_boolean(env_, data->bIsLast, (cbArgs + 3)));

}

void CTPTraderClient::pkg_cb_settlementInfo(OnEventCbRtnField* data, napi_value* cbArgs)
{
	CThostFtdcSettlementInfoField* pSettlementInfo = static_cast<CThostFtdcSettlementInfoField*>(data->rtnField);
	if (pSettlementInfo)
	{
		NAPI_CALL_RETURN_VOID(env_, napi_create_object(env_, cbArgs));


		///交易日
		//TThostFtdcDateType	TradingDay;
		//typedef char TThostFtdcDateType[9];
		napi_value TradingDay;
		NAPI_CALL_RETURN_VOID(env_, napi_create_string_utf8(env_, pSettlementInfo->TradingDay,
			NAPI_AUTO_LENGTH, &TradingDay));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "TradingDay", TradingDay));

		///结算编号
		//TThostFtdcSettlementIDType	SettlementID;
		//typedef int TThostFtdcSettlementIDType;
		napi_value SettlementID;
		NAPI_CALL_RETURN_VOID(env_, napi_create_int32(env_, pSettlementInfo->SettlementID, &SettlementID));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "SettlementID", SettlementID));


		///经纪公司代码
		//TThostFtdcBrokerIDType	BrokerID;
		//typedef char TThostFtdcBrokerIDType[11];
		napi_value BrokerID;
		NAPI_CALL_RETURN_VOID(env_, napi_create_string_utf8(env_, pSettlementInfo->BrokerID,
			NAPI_AUTO_LENGTH, &BrokerID));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "BrokerID", BrokerID));

		///投资者代码
		//TThostFtdcInvestorIDType	InvestorID;
		//typedef char TThostFtdcInvestorIDType[13];
		napi_value InvestorID;
		NAPI_CALL_RETURN_VOID(env_, napi_create_string_utf8(env_, pSettlementInfo->InvestorID,
			NAPI_AUTO_LENGTH, &InvestorID));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "InvestorID", InvestorID));

		///序号
		//TThostFtdcSequenceNoType	SequenceNo;
		//typedef int TThostFtdcSequenceNoType;
		napi_value SequenceNo;
		NAPI_CALL_RETURN_VOID(env_, napi_create_int32(env_, pSettlementInfo->SequenceNo, &SequenceNo));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "SequenceNo", SequenceNo));

		///消息正文
		//TThostFtdcContentType	Content;
		//typedef char TThostFtdcContentType[501];
		//string MsgStr(pSettlementInfo->Content);
		string MsgUTF8 = CHString_To_UTF8(pSettlementInfo->Content);
		napi_value Content;
		NAPI_CALL_RETURN_VOID(env_, napi_create_string_utf8(env_, MsgUTF8.c_str(),
			NAPI_AUTO_LENGTH, &Content));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "Content", Content));

		///资金账号
		//TThostFtdcAccountIDType	AccountID;
		//typedef char TThostFtdcAccountIDType[13];
		napi_value AccountID;
		NAPI_CALL_RETURN_VOID(env_, napi_create_string_utf8(env_, pSettlementInfo->AccountID,
			NAPI_AUTO_LENGTH, &AccountID));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "AccountID", AccountID));


		///币种代码
		//TThostFtdcCurrencyIDType	CurrencyID;
		//typedef char TThostFtdcCurrencyIDType[4];
		napi_value CurrencyID;
		NAPI_CALL_RETURN_VOID(env_, napi_create_string_utf8(env_, pSettlementInfo->CurrencyID,
			NAPI_AUTO_LENGTH, &CurrencyID));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "CurrencyID", CurrencyID));

	}
	else
	{
		NAPI_CALL_RETURN_VOID(env_, napi_get_undefined(env_, cbArgs));
	}

	CThostFtdcRspInfoField *pRspInfo = static_cast<CThostFtdcRspInfoField*>(data->rspInfo);
	pkg_rspinfo(pRspInfo, cbArgs + 1);

	NAPI_CALL_RETURN_VOID(env_, napi_create_int32(env_, data->nRequestID, (cbArgs + 2)));

	NAPI_CALL_RETURN_VOID(env_, napi_get_boolean(env_, data->bIsLast, (cbArgs + 3)));

}

void CTPTraderClient::pkg_cb_confirmsettlement(OnEventCbRtnField* data, napi_value* cbArgs)
{
	CThostFtdcSettlementInfoConfirmField* pSettlementInfoConfirm = static_cast<CThostFtdcSettlementInfoConfirmField*>(data->rtnField);
	if (pSettlementInfoConfirm)
	{
		NAPI_CALL_RETURN_VOID(env_, napi_create_object(env_, cbArgs));

		///经纪公司代码
		//TThostFtdcBrokerIDType	BrokerID;
		//typedef char TThostFtdcBrokerIDType[11];
		napi_value BrokerID;
		NAPI_CALL_RETURN_VOID(env_, napi_create_string_utf8(env_, pSettlementInfoConfirm->BrokerID,
			NAPI_AUTO_LENGTH, &BrokerID));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "BrokerID", BrokerID));
		
		///确认日期
		//TThostFtdcDateType	ConfirmDate;
		//typedef char TThostFtdcDateType[9];
		napi_value ConfirmDate;
		NAPI_CALL_RETURN_VOID(env_, napi_create_string_utf8(env_, pSettlementInfoConfirm->ConfirmDate,
			NAPI_AUTO_LENGTH, &ConfirmDate));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "ConfirmDate", ConfirmDate));

		///确认时间
		//TThostFtdcTimeType	ConfirmTime;
		//typedef char TThostFtdcTimeType[9];
		napi_value ConfirmTime;
		NAPI_CALL_RETURN_VOID(env_, napi_create_string_utf8(env_, pSettlementInfoConfirm->ConfirmTime,
			NAPI_AUTO_LENGTH, &ConfirmTime));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "ConfirmTime", ConfirmTime));

		///投资者代码
		//TThostFtdcInvestorIDType	InvestorID;
		//typedef char TThostFtdcInvestorIDType[13];
		napi_value InvestorID;
		NAPI_CALL_RETURN_VOID(env_, napi_create_string_utf8(env_, pSettlementInfoConfirm->InvestorID,
			NAPI_AUTO_LENGTH, &InvestorID));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "InvestorID", InvestorID));

	}
	else
	{
		NAPI_CALL_RETURN_VOID(env_, napi_get_undefined(env_, cbArgs));
	}

	CThostFtdcRspInfoField *pRspInfo = static_cast<CThostFtdcRspInfoField*>(data->rspInfo);
	pkg_rspinfo(pRspInfo, cbArgs + 1);

	NAPI_CALL_RETURN_VOID(env_, napi_create_int32(env_, data->nRequestID, (cbArgs + 2)));

	NAPI_CALL_RETURN_VOID(env_, napi_get_boolean(env_, data->bIsLast, (cbArgs + 3)));

}

void CTPTraderClient::pkg_cb_userlogin(OnEventCbRtnField* data, napi_value* cbArgs)
{
	CThostFtdcRspUserLoginField* pRspUserLogin = static_cast<CThostFtdcRspUserLoginField*>(data->rtnField);

	if (pRspUserLogin) {

		NAPI_CALL_RETURN_VOID(env_, napi_create_object(env_, cbArgs));

		///交易日
		//TThostFtdcDateType	TradingDay;
		//typedef char TThostFtdcDateType[9];
		napi_value TradingDay;
		NAPI_CALL_RETURN_VOID(env_, napi_create_string_utf8(env_, pRspUserLogin->TradingDay,
			NAPI_AUTO_LENGTH, &TradingDay));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "TradingDay", TradingDay));


		///登录成功时间
		//TThostFtdcTimeType	LoginTime;
		//typedef char TThostFtdcTimeType[9];
		napi_value LoginTime;
		NAPI_CALL_RETURN_VOID(env_, napi_create_string_utf8(env_, pRspUserLogin->LoginTime,
			NAPI_AUTO_LENGTH, &LoginTime));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "LoginTime", LoginTime));

		///经纪公司代码
		//TThostFtdcBrokerIDType	BrokerID;
		//typedef char TThostFtdcBrokerIDType[11];
		napi_value BrokerID;
		NAPI_CALL_RETURN_VOID(env_, napi_create_string_utf8(env_, pRspUserLogin->BrokerID,
			NAPI_AUTO_LENGTH, &BrokerID));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "BrokerID", BrokerID));

		
		///用户代码
		//TThostFtdcUserIDType	UserID;
		//typedef char TThostFtdcUserIDType[16];
		napi_value UserID;
		NAPI_CALL_RETURN_VOID(env_, napi_create_string_utf8(env_, pRspUserLogin->UserID,
			NAPI_AUTO_LENGTH, &UserID));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "UserID", UserID));
		
		
		///交易系统名称
		//TThostFtdcSystemNameType	SystemName;
		//typedef char TThostFtdcSystemNameType[41];
		napi_value SystemName;
		NAPI_CALL_RETURN_VOID(env_, napi_create_string_utf8(env_, pRspUserLogin->SystemName,
			NAPI_AUTO_LENGTH, &SystemName));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "SystemName", SystemName));

		
		///前置编号
		//TThostFtdcFrontIDType	FrontID;
		//typedef int TThostFtdcFrontIDType;
		napi_value FrontID;
		NAPI_CALL_RETURN_VOID(env_, napi_create_int32(env_, pRspUserLogin->FrontID, &FrontID));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "FrontID", FrontID));

		
		///会话编号
		//TThostFtdcSessionIDType	SessionID;
		//typedef int TThostFtdcSessionIDType;
		napi_value SessionID;
		NAPI_CALL_RETURN_VOID(env_, napi_create_int32(env_, pRspUserLogin->SessionID, &SessionID));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "SessionID", SessionID));
		

		///最大报单引用
		//TThostFtdcOrderRefType	MaxOrderRef;
		//typedef char TThostFtdcOrderRefType[13];
		napi_value MaxOrderRef;
		NAPI_CALL_RETURN_VOID(env_, napi_create_string_utf8(env_, pRspUserLogin->MaxOrderRef,
			NAPI_AUTO_LENGTH, &MaxOrderRef));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "MaxOrderRef", MaxOrderRef));

		
		///上期所时间
		//TThostFtdcTimeType	SHFETime;
		//typedef char TThostFtdcTimeType[9];
		napi_value SHFETime;
		NAPI_CALL_RETURN_VOID(env_, napi_create_string_utf8(env_, pRspUserLogin->SHFETime,
			NAPI_AUTO_LENGTH, &SHFETime));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "SHFETime", SHFETime));


		///大商所时间
		//TThostFtdcTimeType	DCETime;
		//typedef char TThostFtdcTimeType[9];
		napi_value DCETime;
		NAPI_CALL_RETURN_VOID(env_, napi_create_string_utf8(env_, pRspUserLogin->DCETime,
			NAPI_AUTO_LENGTH, &DCETime));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "DCETime", DCETime));

		///郑商所时间
		//TThostFtdcTimeType	CZCETime;
		//typedef char TThostFtdcTimeType[9];
		napi_value CZCETime;
		NAPI_CALL_RETURN_VOID(env_, napi_create_string_utf8(env_, pRspUserLogin->CZCETime,
			NAPI_AUTO_LENGTH, &CZCETime));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "CZCETime", CZCETime));

		///中金所时间
		//TThostFtdcTimeType	FFEXTime;
		//typedef char TThostFtdcTimeType[9];
		napi_value FFEXTime;
		NAPI_CALL_RETURN_VOID(env_, napi_create_string_utf8(env_, pRspUserLogin->FFEXTime,
			NAPI_AUTO_LENGTH, &FFEXTime));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "FFEXTime", FFEXTime));


		///能源中心时间
		//TThostFtdcTimeType	INETime;
		//typedef char TThostFtdcTimeType[9];
		napi_value INETime;
		NAPI_CALL_RETURN_VOID(env_, napi_create_string_utf8(env_, pRspUserLogin->INETime,
			NAPI_AUTO_LENGTH, &INETime));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "INETime", INETime));

	}
	else {
		NAPI_CALL_RETURN_VOID(env_, napi_get_undefined(env_, cbArgs));
	}

	CThostFtdcRspInfoField *pRspInfo = static_cast<CThostFtdcRspInfoField*>(data->rspInfo);
	pkg_rspinfo(pRspInfo, cbArgs + 1);

	NAPI_CALL_RETURN_VOID(env_, napi_create_int32(env_, data->nRequestID, (cbArgs + 2)));

	NAPI_CALL_RETURN_VOID(env_, napi_get_boolean(env_, data->bIsLast, (cbArgs + 3)));

}

void CTPTraderClient::pkg_cb_rspauthenticate(OnEventCbRtnField* data, napi_value* cbArgs)
{
	CThostFtdcRspAuthenticateField* pRspAuthenticateField = static_cast<CThostFtdcRspAuthenticateField*>(data->rtnField);
	if (pRspAuthenticateField) {

		NAPI_CALL_RETURN_VOID(env_, napi_create_object(env_, cbArgs));

		///经纪公司代码
		//TThostFtdcBrokerIDType	BrokerID;
		//typedef char TThostFtdcBrokerIDType[11];
		napi_value BrokerID;
		NAPI_CALL_RETURN_VOID(env_, napi_create_string_utf8(env_, pRspAuthenticateField->BrokerID,
			NAPI_AUTO_LENGTH, &BrokerID));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "BrokerID", BrokerID));

		
		///用户代码
		//TThostFtdcUserIDType	UserID;
		//typedef char TThostFtdcUserIDType[16];
		napi_value UserID;
		NAPI_CALL_RETURN_VOID(env_, napi_create_string_utf8(env_, pRspAuthenticateField->UserID,
			NAPI_AUTO_LENGTH, &UserID));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "UserID", UserID));

		///用户端产品信息
		//TThostFtdcProductInfoType	UserProductInfo;
		//typedef char TThostFtdcProductInfoType[11];
		napi_value UserProductInfo;
		NAPI_CALL_RETURN_VOID(env_, napi_create_string_utf8(env_, pRspAuthenticateField->UserProductInfo,
			NAPI_AUTO_LENGTH, &UserProductInfo));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "UserProductInfo", UserProductInfo));

	}
	else
	{
		NAPI_CALL_RETURN_VOID(env_, napi_get_undefined(env_, cbArgs));
	}

	CThostFtdcRspInfoField *pRspInfo = static_cast<CThostFtdcRspInfoField*>(data->rspInfo);
	pkg_rspinfo(pRspInfo, cbArgs + 1);

	NAPI_CALL_RETURN_VOID(env_, napi_create_int32(env_, data->nRequestID, (cbArgs + 2)));

	NAPI_CALL_RETURN_VOID(env_, napi_get_boolean(env_, data->bIsLast, (cbArgs + 3)));

}

void CTPTraderClient::pkg_cb_rsperror(OnEventCbRtnField* data, napi_value* cbArgs) {
	
	CThostFtdcRspInfoField *pRspInfo = static_cast<CThostFtdcRspInfoField*>(data->rspInfo);
	
	pkg_rspinfo(pRspInfo, cbArgs);

	NAPI_CALL_RETURN_VOID(env_, napi_create_int32(env_, data->nRequestID, (cbArgs + 1)));

	NAPI_CALL_RETURN_VOID(env_, napi_get_boolean(env_, data->bIsLast, (cbArgs + 2)));

}

void CTPTraderClient::pkg_rspinfo(CThostFtdcRspInfoField *pRspInfo, napi_value* cbArgs) {
	
	if (pRspInfo) {
		
		NAPI_CALL_RETURN_VOID(env_, napi_create_object(env_, cbArgs));

		///错误代码
		//TThostFtdcErrorIDType	ErrorID;
		//typedef int TThostFtdcErrorIDType;
		napi_value ErrorID;
		NAPI_CALL_RETURN_VOID(env_, napi_create_int32(env_, pRspInfo->ErrorID, &ErrorID));

		NAPI_CALL_RETURN_VOID(env_, napi_set_named_property(env_, *cbArgs, "ErrorID", ErrorID));

		///错误信息
		//TThostFtdcErrorMsgType	ErrorMsg;
		//typedef char TThostFtdcErrorMsgType[81];
		//string MsgStr(pRspInfo->ErrorMsg);
		string MsgUTF8 = CHString_To_UTF8(pRspInfo->ErrorMsg);
		napi_value ErrorMsg;
		NAPI_CALL_RETURN_VOID(env_, napi_create_string_utf8(env_, MsgUTF8.c_str(),
			NAPI_AUTO_LENGTH, &ErrorMsg));

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

string CTPTraderClient::CHString_To_UTF8(char* pszOri)
{
    char pszDst[255] = {0};  
      
	int iLen = Gb2312ToUtf8(pszDst, 50, pszOri, strlen(pszOri));
	
	std::string retStr(pszDst);
	
	return retStr;
}