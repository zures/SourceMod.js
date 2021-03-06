#include "modules/MDota.h"
#include "SMJS_Plugin.h"
#include "modules/MEntities.h"
#include "CDetour/detours.h"
#include "SMJS_Client.h"
#include "SMJS_Entity.h"
#include "SMJS_VKeyValues.h"
#include "sh_memory.h"
#include "game/shared/protobuf/usermessages.pb.h"
#include "game/shared/dota/protobuf/dota_usermessages.pb.h"

#define WAIT_FOR_PLAYERS_COUNT_SIG "\x83\x3D****\x00\x7E\x19\x8B\x0D****\x83\x79\x30\x00"
#define WAIT_FOR_PLAYERS_COUNT_SIG_LEN 19

#define USE_NETPROP_OFFSET(varName, clsName, propName, retFail) static int varName = 0; \
	if(varName == 0){ \
		sm_sendprop_info_t prop; \
		if(!SMJS_Netprops::GetClassPropInfo(#clsName, #propName, &prop)){ \
			printf("Couldn't find " #clsName " in " #propName); \
			RETURN_SCOPED(retFail); \
		} \
		varName = prop.actual_offset; \
	}

#define FIND_DOTA_PTR(name) \
	if(!dotaConf->GetMemSig(#name, (void**) &name) || name == NULL){ \
		smutils->LogError(myself, "Couldn't sigscan " #name); \
	} else { \
		name = *(void**)name; \
	}
	

#define FIND_DOTA_FUNC(name) \
	if(!dotaConf->GetMemSig(#name, (void**) &name) || name == NULL){ \
		smutils->LogError(myself, "Couldn't sigscan " #name); \
	} \

WRAPPED_CLS_CPP(MDota, SMJS_Module);

static IGameConfig *dotaConf = NULL;
static void *LoadParticleFile;
static void *CreateUnit;

static void *GameManager;

static int waitingForPlayersCount = 10;
static int* waitingForPlayersCountPtr = NULL;
static int* expRequiredForLevel = NULL;

static CDetour *parseUnitDetour;
static CDetour *getAbilityValueDetour;
static CDetour *clientPickHeroDetour;
static CDetour *heroBuyItemDetour;

static void* (*FindClearSpaceForUnit)(void *unit, Vector vec, int bSomething);
static CBaseEntity* (*DCreateItem)(const char *item, void *unit, void *unit2);
static int (__stdcall *DGiveItem)(CBaseEntity *inventory, int a4, int a5, char a6); // eax = 0, ecx = item
static void (*DDestroyItem)(CBaseEntity *item);
static int (__stdcall *StealAbility)(CBaseEntity *hero, CBaseEntity *newAbility, int slot, int somethingUsuallyZero);
static CBaseEntity* (*DCreateItemDrop)(Vector location);
static void **DLinkItemDrop;


static void PatchVersionCheck();
static void PatchWaitForPlayersCount();

enum FieldType {
	FT_Void = 0,
	FT_Float = 1,
	FT_Int = 5
};

struct AbilityField {
	uint32_t flags1; // EE FF EE FF (LE)
	union {
		int32_t asInt;
		float asFloat;
	} value;
		
	uint32_t flags2; // EE FF EE FF (LE)
	FieldType type;
};

struct AbilityData {
	char *field;
	char *strValue;
	char *unknown;

	struct {
		int32_t		something;
		FieldType	type;
	} root;

	AbilityField values[12];

	uint32_t	flags;
};

#include "modules/MDota_Detours.h"

MDota::MDota(){
	identifier = "dota";

	PatchVersionCheck();
	PatchWaitForPlayersCount();

	char conf_error[255] = "";
	if (!gameconfs->LoadGameConfigFile("smjs.dota", &dotaConf, conf_error, sizeof(conf_error))){
		if (conf_error){
			smutils->LogError(myself, "Could not read smjs.dota.txt: %s", conf_error);
			return;
		}
	}

	CDetourManager::Init(g_pSM->GetScriptingEngine(), dotaConf);


	parseUnitDetour = DETOUR_CREATE_MEMBER(ParseUnit, "ParseUnit");
	if(parseUnitDetour) parseUnitDetour->EnableDetour();

	getAbilityValueDetour = DETOUR_CREATE_STATIC(GetAbilityValue, "GetAbilityValue");
	if(getAbilityValueDetour) getAbilityValueDetour->EnableDetour();

	clientPickHeroDetour = DETOUR_CREATE_STATIC(ClientPickHero, "ClientPickHero");
	if(clientPickHeroDetour) clientPickHeroDetour->EnableDetour();
	
	heroBuyItemDetour = DETOUR_CREATE_STATIC(HeroBuyItem, "HeroBuyItem");
	if(heroBuyItemDetour) heroBuyItemDetour->EnableDetour();

	FIND_DOTA_PTR(GameManager);

	FIND_DOTA_FUNC(LoadParticleFile);
	FIND_DOTA_FUNC(CreateUnit);
	FIND_DOTA_FUNC(FindClearSpaceForUnit);
	FIND_DOTA_FUNC(DCreateItem);
	FIND_DOTA_FUNC(DGiveItem);
	FIND_DOTA_FUNC(DDestroyItem);
	FIND_DOTA_FUNC(StealAbility);
	FIND_DOTA_FUNC(DCreateItemDrop);
	FIND_DOTA_FUNC(DLinkItemDrop);

	expRequiredForLevel = (int*) memutils->FindPattern(g_SMAPI->GetServerFactory(false), "\x00\x00\x00\x00\xC8\x00\x00\x00\xF4\x01\x00\x00\x84\x03\x00\x00\x78\x05\x00\x00", 20);
	if(expRequiredForLevel == NULL){
		smutils->LogError(myself, "Couldn't find expRequiredForLevel\n");
	}
}

void MDota::OnWrapperAttached(SMJS_Plugin *plugin, v8::Persistent<v8::Value> wrapper){
	auto obj = wrapper->ToObject();
	
}

void PatchVersionCheck(){
	uint8_t *ptr = (uint8_t*) memutils->FindPattern(g_SMAPI->GetServerFactory(false), 
	"\x8B\x2A\x2A\x2A\x2A\x2A\x8B\x11\x8B\x82\x1C\x02\x00\x00\xFF\xD0\x8B\x2A\x2A\x2A\x2A\x2A"
	"\x50\x51\x68\x2A\x2A\x2A\x2A\xFF\x2A\x2A\x2A\x2A\x2A\x8B\x2A\x2A\x2A\x2A\x2A\x8B\x11\x8B"
	"\x82\x98\x00\x00\x00\x83\xC4\x0C\x68\x2A\x2A\x2A\x2A\xFF\xD0", 59);

	if(ptr == NULL){
		printf("Failed to patch version check!\n");
		smutils->LogError(myself, "Failed to patch version check!");
		return;
	}

	SourceHook::SetMemAccess(ptr, 59, SH_MEM_READ | SH_MEM_WRITE | SH_MEM_EXEC);

	for(int i = 35; i < 59; ++i){
		ptr[i] = 0x90; // NOP
	}

	ptr = (uint8_t*) memutils->FindPattern(g_SMAPI->GetServerFactory(false), 
	"\x8B\x11\x8B\x82\x1C\x02\x00\x00\xFF\xD0\x8B\x2A\x2A\x2A\x2A\x2A\x50\x51\x68\x2A\x2A\x2A\x2A\xFF\x2A\x2A\x2A\x2A\x2A"
	"\x83\xC4\x0C\x38\x2A\x2A\x2A\x2A\x2A\x74\x50", 40);

	if(ptr == NULL){
		printf("Failed to patch version check!\n");
		smutils->LogError(myself, "Failed to patch version check!");
		return;
	}

	SourceHook::SetMemAccess(ptr, 40, SH_MEM_READ | SH_MEM_WRITE | SH_MEM_EXEC);

	ptr[38] = 0xEB; // JZ --> JMP
}

void PatchWaitForPlayersCount(){
	uint8_t *ptr = (uint8_t*) memutils->FindPattern(g_SMAPI->GetServerFactory(false), WAIT_FOR_PLAYERS_COUNT_SIG, WAIT_FOR_PLAYERS_COUNT_SIG_LEN);

	if(ptr == NULL){
		printf("Failed to patch dota_wait_for_players_to_load_count\n");
		return;
	}

	SourceHook::SetMemAccess(ptr, WAIT_FOR_PLAYERS_COUNT_SIG_LEN, SH_MEM_READ | SH_MEM_WRITE | SH_MEM_EXEC);
	
	waitingForPlayersCountPtr = *((int **)((intptr_t) ptr + 2));
	*waitingForPlayersCountPtr = waitingForPlayersCount;
	*((int **)((intptr_t) ptr + 2)) = &waitingForPlayersCount;
}

FUNCTION_M(MDota::loadParticleFile)
	ARG_COUNT(1);
	PSTR(file);
	char *fileStr = *file;
	__asm{
		mov eax, fileStr
		call LoadParticleFile
	}

	RETURN_UNDEF;
END

const char* MDota::HeroIdToClassname(int id) {
	switch(id){
		case Hero_Base: return "npc_dota_hero_base";
		case Hero_AntiMage: return "npc_dota_hero_antimage";
		case Hero_Axe: return "npc_dota_hero_axe";
		case Hero_Bane: return "npc_dota_hero_bane";
		case Hero_Bloodseeker: return "npc_dota_hero_bloodseeker";
		case Hero_CrystalMaiden: return "npc_dota_hero_crystal_maiden";
		case Hero_DrowRanger: return "npc_dota_hero_drow_ranger";
		case Hero_EarthShaker: return "npc_dota_hero_earthshaker";
		case Hero_Juggernaut: return "npc_dota_hero_juggernaut";
		case Hero_Mirana: return "npc_dota_hero_mirana";
		case Hero_Morphling: return "npc_dota_hero_morphling";
		case Hero_ShadowFiend: return "npc_dota_hero_nevermore";
		case Hero_PhantomLancer: return "npc_dota_hero_phantom_lancer";
		case Hero_Puck: return "npc_dota_hero_puck";
		case Hero_Pudge: return "npc_dota_hero_pudge";
		case Hero_Razor: return "npc_dota_hero_razor";
		case Hero_SandKing: return "npc_dota_hero_sand_king";
		case Hero_StormSpirit: return "npc_dota_hero_storm_spirit";
		case Hero_Sven: return "npc_dota_hero_sven";
		case Hero_Tiny: return "npc_dota_hero_tiny";
		case Hero_VengefulSpirit: return "npc_dota_hero_vengefulspirit";
		case Hero_Windrunner: return "npc_dota_hero_windrunner";
		case Hero_Zeus: return "npc_dota_hero_zuus";
		case Hero_Kunkka: return "npc_dota_hero_kunkka";
		case Hero_Lina: return "npc_dota_hero_lina";
		case Hero_Lion: return "npc_dota_hero_lion";
		case Hero_ShadowShaman: return "npc_dota_hero_shadow_shaman";
		case Hero_Slardar: return "npc_dota_hero_slardar";
		case Hero_Tidehunter: return "npc_dota_hero_tidehunter";
		case Hero_WitchDoctor: return "npc_dota_hero_witch_doctor";
		case Hero_Lich: return "npc_dota_hero_lich";
		case Hero_Riki: return "npc_dota_hero_riki";
		case Hero_Enigma: return "npc_dota_hero_enigma";
		case Hero_Tinker: return "npc_dota_hero_tinker";
		case Hero_Sniper: return "npc_dota_hero_sniper";
		case Hero_Necrolyte: return "npc_dota_hero_necrolyte";
		case Hero_Warlock: return "npc_dota_hero_warlock";
		case Hero_BeastMaster: return "npc_dota_hero_beastmaster";
		case Hero_QueenOfPain: return "npc_dota_hero_queenofpain";
		case Hero_Venomancer: return "npc_dota_hero_venomancer";
		case Hero_FacelessVoid: return "npc_dota_hero_faceless_void";
		case Hero_SkeletonKing: return "npc_dota_hero_skeleton_king";
		case Hero_DeathProphet: return "npc_dota_hero_death_prophet";
		case Hero_PhantomAssassin: return "npc_dota_hero_phantom_assassin";
		case Hero_Pugna: return "npc_dota_hero_pugna";
		case Hero_TemplarAssassin: return "npc_dota_hero_templar_assassin";
		case Hero_Viper: return "npc_dota_hero_viper";
		case Hero_Luna: return "npc_dota_hero_luna";
		case Hero_DragonKnight: return "npc_dota_hero_dragon_knight";
		case Hero_Dazzle: return "npc_dota_hero_dazzle";
		case Hero_Clockwerk: return "npc_dota_hero_rattletrap";
		case Hero_Leshrac: return "npc_dota_hero_leshrac";
		case Hero_Furion: return "npc_dota_hero_furion";
		case Hero_Lifestealer: return "npc_dota_hero_life_stealer";
		case Hero_DarkSeer: return "npc_dota_hero_dark_seer";
		case Hero_Clinkz: return "npc_dota_hero_clinkz";
		case Hero_Omniknight: return "npc_dota_hero_omniknight";
		case Hero_Enchantress: return "npc_dota_hero_enchantress";
		case Hero_Huskar: return "npc_dota_hero_huskar";
		case Hero_NightStalker: return "npc_dota_hero_night_stalker";
		case Hero_Broodmother: return "npc_dota_hero_broodmother";
		case Hero_BountyHunter: return "npc_dota_hero_bounty_hunter";
		case Hero_Weaver: return "npc_dota_hero_weaver";
		case Hero_Jakiro: return "npc_dota_hero_jakiro";
		case Hero_Batrider: return "npc_dota_hero_batrider";
		case Hero_Chen: return "npc_dota_hero_chen";
		case Hero_Spectre: return "npc_dota_hero_spectre";
		case Hero_AncientApparition: return "npc_dota_hero_ancient_apparition";
		case Hero_Doom: return "npc_dota_hero_doom_bringer";
		case Hero_Ursa: return "npc_dota_hero_ursa";
		case Hero_SpiritBreaker: return "npc_dota_hero_spirit_breaker";
		case Hero_Gyrocopter: return "npc_dota_hero_gyrocopter";
		case Hero_Alchemist: return "npc_dota_hero_alchemist";
		case Hero_Invoker: return "npc_dota_hero_invoker";
		case Hero_Silencer: return "npc_dota_hero_silencer";
		case Hero_ObsidianDestroyer: return "npc_dota_hero_obsidian_destroyer";
		case Hero_Lycan: return "npc_dota_hero_lycan";
		case Hero_Brewmaster: return "npc_dota_hero_brewmaster";
		case Hero_ShadowDemon: return "npc_dota_hero_shadow_demon";
		case Hero_LoneDruid: return "npc_dota_hero_lone_druid";
		case Hero_ChaosKnight: return "npc_dota_hero_chaos_knight";
		case Hero_Meepo: return "npc_dota_hero_meepo";
		case Hero_TreantProtector: return "npc_dota_hero_treant";
		case Hero_OgreMagi: return "npc_dota_hero_ogre_magi";
		case Hero_Undying: return "npc_dota_hero_undying";
		case Hero_Rubick: return "npc_dota_hero_rubick";
		case Hero_Disruptor: return "npc_dota_hero_disruptor";
		case Hero_NyxAssassin: return "npc_dota_hero_nyx_assassin";
		case Hero_NagaSiren: return "npc_dota_hero_naga_siren";
		case Hero_KeeperOfTheLight: return "npc_dota_hero_keeper_of_the_light";
		case Hero_Wisp: return "npc_dota_hero_wisp";
		case Hero_Visage: return "npc_dota_hero_visage";
		case Hero_Slark: return "npc_dota_hero_slark";
		case Hero_Medusa: return "npc_dota_hero_medusa";
		case Hero_TrollWarlord: return "npc_dota_hero_troll_warlord";
		case Hero_CentaurWarchief: return "npc_dota_hero_centaur";
		case Hero_Magnus: return "npc_dota_hero_magnataur";
		case Hero_Timbersaw: return "npc_dota_hero_shredder";
		case Hero_Bristleback: return "npc_dota_hero_bristleback";
		case Hero_Tusk: return "npc_dota_hero_tusk";
		case Hero_SkywrathMage: return "npc_dota_hero_skywrath_mage";
		case Hero_Abaddon: return "npc_dota_hero_abaddon";
		case Hero_ElderTitan: return "npc_dota_hero_elder_titan";
		case Hero_LegionCommander: return "npc_dota_hero_legion_commander";
	}

	return NULL;
}

FUNCTION_M(MDota::heroIdToClassname)
	PINT(id);
	auto ret = HeroIdToClassname(id);
	if(ret == NULL) return v8::Null();
	RETURN_SCOPED(v8::String::New(ret));
END

FUNCTION_M(MDota::forceWin)
	PINT(team);
	if(team != 2 && team != 3) THROW_VERB("Invalid team %d", team);

	//FIXME
	auto cmd = icvar->FindCommand("dota_kill_buildings");
	cmd->RemoveFlags(FCVAR_CHEAT);
	engine->ServerCommand("dota_kill_buildings\n");

	RETURN_UNDEF;
END

FUNCTION_M(MDota::createUnit)
	PSTR(tmp);
	PINT(team);
	
	CBaseEntity *ent;
	CBaseEntity *targetEntity;
	char *clsname = *tmp;
	

	if(args.Length() > 2){
		POBJ(otherTmp);
		auto inte = otherTmp->GetInternalField(0);
		if(inte.IsEmpty()){
			THROW("Invalid other entity");
		}

		auto other = dynamic_cast<SMJS_Entity*>((SMJS_Base*) v8::Handle<v8::External>::Cast(inte)->Value());
		if(other == NULL) THROW("Invalid other entity");
		targetEntity = other->ent;
	}else{
		targetEntity = gamehelpers->EdictOfIndex(0)->GetNetworkable()->GetBaseEntity();
	}

	__asm {
		mov		eax, clsname
		push	0
		push	1
		push	team
		push	targetEntity
		call	CreateUnit
		mov		ent, eax
		add		esp, 10h
	}
	
	if(ent == NULL) return v8::Null();

	RETURN_SCOPED(GetEntityWrapper(ent)->GetWrapper(GetPluginRunning()));
END

FUNCTION_M(MDota::findClearSpaceForUnit)
	POBJ(otherTmp);
	PVEC(x, y, z);

	CBaseEntity *targetEntity;
	auto inte = otherTmp->GetInternalField(0);
	if(inte.IsEmpty()){
		THROW("Invalid other entity");
	}

	auto other = dynamic_cast<SMJS_Entity*>((SMJS_Base*) v8::Handle<v8::External>::Cast(inte)->Value());
	if(other == NULL) THROW("Invalid other entity");
	targetEntity = other->ent;

	FindClearSpaceForUnit(targetEntity, Vector(x, y, z), true);
	RETURN_UNDEF;
END

FUNCTION_M(MDota::setWaitForPlayersCount)
	if(GetPluginRunning()->IsSandboxed()) THROW("This function is not allowed to be called in sandboxed plugins");	

	PINT(c);

	waitingForPlayersCount = c;
	if(waitingForPlayersCountPtr != NULL){
		*waitingForPlayersCountPtr = c;
	}

	auto d2fixupsConVar = icvar->FindVar("dota_wait_for_players_to_load_count");
	
	if(d2fixupsConVar != NULL){
		d2fixupsConVar->SetValue(c);
	}

	RETURN_UNDEF;
END

FUNCTION_M(MDota::giveItemToHero)
	USE_NETPROP_OFFSET(offset, CDOTA_BaseNPC, m_Inventory, v8::Null());

	PSTR(itemClsname);
	POBJ(unit);
	PENT(ent);
	if(ent == NULL){
		THROW("Must provide a valid hero");
	}

	auto item = DCreateItem(*itemClsname, ent->ent, ent->ent);
	if(item == NULL){
		RETURN_SCOPED(v8::Null());
	}

	auto inventory = (CBaseEntity*)((intptr_t)ent->ent + offset);

	int res;

	__asm{
		push	0
		push	-1
		push	3
		push	inventory
		mov		eax, 0
		mov		ecx, item
		call	DGiveItem
		mov		res, eax
	}

	if(res != -1) {
		DDestroyItem(item);
		RETURN_SCOPED(v8::Null());
	}

	RETURN_SCOPED(GetEntityWrapper(item)->GetWrapper(GetPluginRunning()));
END


FUNCTION_M(MDota::getTotalExpRequiredForLevel)
	if(expRequiredForLevel == NULL){
		RETURN_INT(0);
	}

	PINT(level);

	if(level < 1) THROW_VERB("Invalid level", level);
	if(level > 25) THROW_VERB("Invalid level", level);
	RETURN_INT(expRequiredForLevel[level - 1]);
END

FUNCTION_M(MDota::setTotalExpRequiredForLevel)
	if(expRequiredForLevel == NULL) RETURN_UNDEF;

	PINT(level);
	PINT(exp);

	if(level <= 1) THROW_VERB("Invalid level", level);
	if(level > 25) THROW_VERB("Invalid level", level);

	expRequiredForLevel[level - 1] = exp;

	RETURN_UNDEF;
END

FUNCTION_M(MDota::sendStatPopup)
	USE_NETPROP_OFFSET(offset, CDOTAPlayer, m_iPlayerID, v8::Boolean::New(false));
	
	POBJ(targetObj);

	auto target = dynamic_cast<SMJS_Client*>((SMJS_Base*) v8::Handle<v8::External>::Cast(targetObj->GetInternalField(0))->Value());
	if(target == NULL) THROW("Invalid target");


	PINT(type);
	if(args.Length() < 3) THROW("Argument 3 must be an array of strings");
	POBJ(tmpObj);
	if(!tmpObj->IsArray()) THROW("Argument 3 must be an array of strings");

	auto arr = v8::Handle<v8::Array>::Cast(tmpObj);

	CDOTAUserMsg_SendStatPopup msg;
	msg.set_player_id(*(int*)((intptr_t) target + offset));

	for(uint32_t i = 0, len = arr->Length(); i < len; ++i){
		v8::String::Utf8Value str(arr->Get(i));
		msg.mutable_statpopup()->add_stat_strings(*str, str.length());
	}

	SingleRecipientFilter filter(target->entIndex);
	engine->SendUserMessage(filter, DOTA_UM_SendStatPopup, msg);
	
	RETURN_SCOPED(v8::Boolean::New(true));
END

FUNCTION_M(MDota::setHeroAvailable)
	USE_NETPROP_OFFSET(stableOffset, CDOTAGameManagerProxy, m_StableHeroAvailable, v8::Undefined());
	USE_NETPROP_OFFSET(currentOffset, CDOTAGameManagerProxy, m_CurrentHeroAvailable, v8::Undefined());
	USE_NETPROP_OFFSET(culledOffset, CDOTAGameManagerProxy, m_CulledHeroes, v8::Undefined());
	
	PINT(hero);
	PBOL(available);
	*(bool*)((intptr_t) GameManager + stableOffset + hero) = available;
	*(bool*)((intptr_t) GameManager + currentOffset + hero) = available;
	*(bool*)((intptr_t) GameManager + culledOffset + hero) = available;
	RETURN_UNDEF;
END

FUNCTION_M(MDota::createItemDrop)
	PENT(owner);
	PSTR(itemName);
	PVEC(x, y, z);

	CBaseEntity *pEntity;

	if(owner == NULL){
		THROW("Item must have an owner");
	}else{
		pEntity = owner->ent;
	}

	auto item = DCreateItem(*itemName, NULL, pEntity);
	if(item == NULL){
		RETURN_SCOPED(v8::Null());
	}

	auto drop = DCreateItemDrop(Vector(x, y, z));
	if(drop == NULL){
		DDestroyItem(item);
		RETURN_SCOPED(v8::Null());
	}

	__asm {
		mov			eax, item
		push		drop
		call		DLinkItemDrop
	}

	RETURN_SCOPED(GetEntityWrapper(drop)->GetWrapper(GetPluginRunning()));
END