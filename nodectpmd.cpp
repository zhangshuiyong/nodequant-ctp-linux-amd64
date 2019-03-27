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

//------------------libuv channel机制 子线程通知主线程uv_async_send------>主线程MainThreadCallback

CTPMarketDataClient::CTPMarketDataClient() : env_(nullptr), wrapper_(nullptr)
{
	//channel的data固定不变, 为当前实例
	channel.data = this;
	//打开libuv通道
	uv_async_init(uv_default_loop(), &channel, (uv_async_cb)CTPMarketDataClient::MainThreadCallback);
	//激活互斥锁
	uv_mutex_init(&eventQueueMutex);
}

//子线程
void CTPMarketDataClient::queueEvent(OnEventCbRtnField* event)
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
void CTPMarketDataClient::MainThreadCallback(uv_async_t* channel) {
	CTPMarketDataClient *marketDataClient = (CTPMarketDataClient*)channel->data;

	marketDataClient->processEventQueue();
}

//主线程
void CTPMarketDataClient::processEventQueue() {
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

CTPMarketDataClient::~CTPMarketDataClient()
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
	uv_close((uv_handle_t*)&channel, (uv_close_cb)CTPMarketDataClient::ChannelClosedCallback);
	//销毁事件队列的互斥锁
	uv_mutex_destroy(&eventQueueMutex);

	//释放后单例模式
	napi_delete_reference(env_, Singleton);
	Singleton = nullptr;

	//对象销毁，不需要销毁类的prototype对象引用，它跟随进程销毁
	//napi_delete_reference(env_, prototype);
}

void CTPMarketDataClient::ChannelClosedCallback(uv_async_t* pChannel) {
	
}

//------------------libuv channel机制 子线程通知主线程uv_async_send------>主线程MainThreadCallback

void CTPMarketDataClient::Destructor(napi_env env,
	void* nativeObject,
	void* /*finalize_hint*/) {
	CTPMarketDataClient* obj = static_cast<CTPMarketDataClient*>(nativeObject);
	delete obj;
}

//创建类的静态区 static prototype对象
napi_status CTPMarketDataClient::Init(napi_env env)
{
	//初始化static变量eventName_map,规定所有的事件，不允许未定义的事件
	if (eventName_map.size() == 0)
		initEventNameMap();

	napi_status status;
	//所有的Js函数在Native层都是static声明的函数，napi_callback函数类型
	//因为c++对象需要被Js层对象Wrap(包裹隐藏)
	//导致在Js层不能看到和访问c++对象的任何属性(公有或私有或方法)
	//只能把c++类的static函数（可访问本类对象的私有属性）声明为napi_callback类型
	//创建为Js层函数并且绑定到c++类中static的prototype对象的属性上
	//通过Js对象调用prototype函数来访问被Wrap的原来c++对象
	//调用myObject.on()
	//1.on是一个Js层函数
	//2.on是MyObject类中的static函数
	//3.只有MyObject类的实例myObject才能作为on Js函数的call对象
	//4.满足1,2,3,on Js函数被v8执行
	napi_property_descriptor properties[] = {
		DECLARE_NAPI_PROPERTY("on", on),
		DECLARE_NAPI_PROPERTY("connect", connect),
		DECLARE_NAPI_PROPERTY("login", login),
		DECLARE_NAPI_PROPERTY("subscribeMarketData", subscribeMarketData),
		DECLARE_NAPI_PROPERTY("unSubscribeMarketData", unSubscribeMarketData),
		DECLARE_NAPI_PROPERTY("logout", logout),
		DECLARE_NAPI_PROPERTY("getTradingDay", getTradingDay)
	};

	//创建Js对象prototype
	napi_value _proto_;
	//1.创建prototype Js对象
	//2.创建New函数为Js层函数,并且命名
	//3.给prototype对象的constructor属性指向New函数
	//4.给prototype对象绑定多个属性
	status = napi_define_class(
		env, "CTPMarketDataClient", NAPI_AUTO_LENGTH, New, nullptr, 
		sizeof(properties) / sizeof(*properties), properties, &_proto_);
	if (status != napi_ok) return status;

	//防止销毁，为prototype创建ref对象，驻存到进程的静态区中
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

	//已经设置了,不再更改响应函数
	if (nullptr != callback_map[eventItem->second]) {
		return NULL;
	}

	NAPI_CALL(env, napi_create_reference(env, args[1], 1, &callback_map[eventItem->second]));

	return NULL;
}

//连接交易所前台
//连接成功时：onFrontConnected回调函数
//断开连接时：onFrontDisconnected回调函数
//应答错误：onRspError回调函数
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

	//Js主线程请求号递增
	requestID++;
	//c++ void*泛型指针
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

	//_completed完成函数中需要销毁
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


	CTPMarketDataClient* marketDataClient;
	NAPI_CALL(env, napi_unwrap(env, _this, reinterpret_cast<void**>(&marketDataClient)));

	//Js主线程请求号递增
	requestID++;
	//c++ void*泛型指针
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


	//_completed完成函数中需要销毁
	CThostFtdcUserLogoutField req;
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


	CTPMarketDataClient* marketDataClient;
	NAPI_CALL(env, napi_unwrap(env, _this, reinterpret_cast<void**>(&marketDataClient)));

	//Js主线程请求号递增
	requestID++;
	//c++ void*泛型指针
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

	//Js主线程请求号递增
	requestID++;
	//c++ void*泛型指针
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

	//Js主线程请求号递增
	requestID++;
	//c++ void*泛型指针
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

//////////////////////////////////////////////////////////产生多个Spi线程相应调用////////////////////////////////////////////////////////////////////////////////

void CTPMarketDataClient::OnFrontConnected()
{
	OnEventCbRtnField* field = new OnEventCbRtnField(); //调用完毕后需要销毁
	field->eFlag = T_On_FrontConnected;                 //FrontConnected
	queueEvent(field);                                  //对象销毁后，指针清空
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


//主要在Js环境中转换和调用Js层callback函数
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
		
		//错误信息
		//typedef char TThostFtdcErrorMsgType[81];
		//单字符汉字char数组->utf8字符串
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
