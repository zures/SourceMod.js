#include "MClient.h"
#include "SMJS_Plugin.h"
#include "MEntities.h"

SH_DECL_HOOK5(IServerGameClients, ClientConnect, SH_NOATTRIB, 0, bool, int, const char *, const char *, char *, int);
SH_DECL_HOOK2_void(IServerGameClients, ClientPutInServer, SH_NOATTRIB, 0, int, const char *);
SH_DECL_HOOK1_void(IServerGameClients, ClientDisconnect, SH_NOATTRIB, 0, int);
SH_DECL_HOOK2_void(IServerGameClients, ClientCommand, SH_NOATTRIB, 0, int, const CCommand &);
SH_DECL_HOOK1_void(IServerGameClients, ClientSettingsChanged, SH_NOATTRIB, 0, int);

bool OnClientConnect(int client, const char *pszName, const char *pszAddress, char *reject, int maxrejectlen);
bool OnClientConnect_Post(int client, const char *pszName, const char *pszAddress, char *reject, int maxrejectlen);
void OnClientPutInServer(int client, const char *playername);
void OnClientDisconnect(int clientIndex);
void OnClientDisconnect_Post(int clientIndex);
void OnClientCommand(int client, const CCommand &args);

SMJS_Client *clients[MAXCLIENTS];

WRAPPED_CLS_CPP(MClient, SMJS_Module)

MClient *self;

MClient::MClient(){
	self = this;
	

	identifier = "clients";
	SH_ADD_HOOK(IServerGameClients, ClientConnect, serverClients, SH_STATIC(OnClientConnect), false);
	SH_ADD_HOOK(IServerGameClients, ClientConnect, serverClients, SH_STATIC(OnClientConnect_Post), true);

	
	SH_ADD_HOOK(IServerGameClients, ClientPutInServer, serverClients, SH_STATIC(OnClientPutInServer), true);
	SH_ADD_HOOK(IServerGameClients, ClientDisconnect, serverClients, SH_STATIC(OnClientDisconnect), false);
	SH_ADD_HOOK(IServerGameClients, ClientDisconnect, serverClients, SH_STATIC(OnClientDisconnect_Post), true);
	SH_ADD_HOOK(IServerGameClients, ClientCommand, serverClients, SH_STATIC(OnClientCommand), false);

	//SH_ADD_HOOK(IServerGameClients, ClientSettingsChanged, serverClients, SH_STATIC(OnClientSettingsChanged), true);

	smutils->AddGameFrameHook(MClient::OnGameFrame);
}

void MClient::OnWrapperAttached(SMJS_Plugin *plugin, v8::Persistent<v8::Value> wrapper){
	auto obj = wrapper->ToObject();

	plugin->GetContext()->Global()->Set(v8::String::NewSymbol("Client"), SMJS_Client::GetTemplateForPlugin(plugin)->GetFunction());
}

void MClient::OnGameFrame(bool simulating){
	RunAuthChecks();
}

void MClient::RunAuthChecks(){
	for(int i = 0; i < sizeof(clients) / sizeof(clients[0]); ++i){
		auto client = clients[i];
		if(client == NULL) continue;
		if(!client->connected) continue;
		if(client->authStage == 2) continue;
		
		if(client->authStage == 0){
			const char *authid = engine->GetPlayerNetworkIDString(client->entIndex);
			if(authid == NULL) continue;
			client->authStage = 1;

			self->CallGlobalFunctionWithWrapped("OnClientPreAuthorized", client);
		}

		if(client->authStage == 1){
			if(engine->IsClientFullyAuthenticated(client->entIndex)){
				client->authStage = 2;
				self->CallGlobalFunctionWithWrapped("OnClientAuthorized", client);
			}
		}
	}
	
}

bool OnClientConnect(int clientIndex, const char *pszName, const char *pszAddress, char *reject, int maxrejectlen){
	edict_t *pEntity = gamehelpers->EdictOfIndex(clientIndex);
	auto client = new SMJS_Client(pEntity);
	clients[client->entIndex] = client;

	SMJS_Plugin *pl = NULL;
	auto returnValue = self->CallGlobalFunctionWithWrapped("OnClientConnect", client, &pl, true);
	if(pl == NULL) return true;

	HandleScope handle_scope(pl->GetIsolate());
	Context::Scope context_scope(pl->GetContext());

	if(!returnValue->IsUndefined() && !returnValue->IsNull()){
		snprintf(reject, maxrejectlen, "%s", *v8::String::Utf8Value(returnValue->ToString()));
		RETURN_META_VALUE(MRES_SUPERCEDE, false);
	}
	
	
	return true;
}

bool OnClientConnect_Post(int clientIndex, const char *pszName, const char *pszAddress, char *reject, int maxrejectlen){
	auto client = clients[clientIndex];
	client->connected = true;
	self->CallGlobalFunctionWithWrapped("OnClientConnected", client);
	return true;
}

void OnClientPutInServer(int clientIndex, const char *playername){
	auto client = clients[clientIndex];

	// If it's a bot
	if(client == NULL){
		edict_t *pEntity = gamehelpers->EdictOfIndex(clientIndex);
		client = new SMJS_Client(pEntity);
		clients[client->entIndex] = client;
		client->ReattachEntity();
		clients[gamehelpers->IndexOfEdict(pEntity)] = client;
	}

	client->inGame = true;
	client->ReattachEntity();
	SetEntityWrapper(client->ent, client);
	self->CallGlobalFunctionWithWrapped("OnClientPutInServer", client);
}

int clientIndexThatJustDisconnected;
void OnClientDisconnect(int clientIndex){
	auto client = clients[clientIndex];
	self->CallGlobalFunctionWithWrapped("OnClientDisconnect", client);
}

void OnClientDisconnect_Post(int clientIndex){
	auto client = clients[clientIndex];

	// It seems OnClientDisconnect_Post is called multiple times, if it's already been called, don't run
	// this function again
	if(client == NULL) return;

	client->ent = NULL;
	client->valid = false;
	self->CallGlobalFunctionWithWrapped("OnClientDisconnected", client);

	client->entIndex = -1;
	client->Destroy();
	clients[clientIndex] = NULL;
}

void OnClientCommand(int client, const CCommand &args){

}