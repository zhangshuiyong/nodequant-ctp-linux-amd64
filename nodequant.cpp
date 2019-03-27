#include <node_api.h>
#include "common.h"

#include "nodectpmd.h"

#include "nodectptd.h"

static napi_value CreateSingletonTdClient(napi_env env, napi_callback_info info) {
	if (CTPTraderClient::Singleton == nullptr) {

		napi_value instance;
		NAPI_CALL(env, CTPTraderClient::NewInstance(env, nullptr, &instance));

		NAPI_CALL(env, napi_create_reference(env, instance,1, &CTPTraderClient::Singleton));
		return instance;
	}
	else {

		napi_value instance;

		NAPI_CALL(env,napi_get_reference_value(env, CTPTraderClient::Singleton, &instance));

		return instance;
	}
}

napi_value CreateSingletonMdClient(napi_env env, napi_callback_info info) {
	if (CTPMarketDataClient::Singleton == nullptr) {
		napi_value instance;
		NAPI_CALL(env, CTPMarketDataClient::NewInstance(env, nullptr, &instance));

		NAPI_CALL(env, napi_create_reference(env, instance, 1, &CTPMarketDataClient::Singleton));

		return instance;
	}
	else {
		napi_value instance;

		NAPI_CALL(env, napi_get_reference_value(env, CTPMarketDataClient::Singleton, &instance));

		return instance;
	}
}

napi_value Init(napi_env env, napi_value exports) {

	NAPI_CALL(env, CTPMarketDataClient::Init(env));

	NAPI_CALL(env, CTPTraderClient::Init(env));

	napi_property_descriptor CreateSingletonMdApiDesc = DECLARE_NAPI_PROPERTY("SingletonMdClient", CreateSingletonMdClient);
	napi_property_descriptor CreateSingletonTdApiDesc = DECLARE_NAPI_PROPERTY("SingletonTdClient", CreateSingletonTdClient);

	napi_property_descriptor descriptors[] = { CreateSingletonMdApiDesc,CreateSingletonTdApiDesc };
	
	NAPI_CALL(env, napi_define_properties(env, exports, 2, descriptors));

	return exports;

}

NAPI_MODULE(NODE_GYP_MODULE_NAME, Init)