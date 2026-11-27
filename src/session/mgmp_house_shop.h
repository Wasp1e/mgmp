#pragma once
#include <cstdint>
namespace mgmp {
struct ChoiceMsg;
struct LogFileMsg;
constexpr uintptr_t kHouseStoreStorageUpgrade = 0xBC;
using HouseShopBuy = void* (__fastcall*)(void*, void*);
void house_shop_set_base(uintptr_t base);
// Main-thread boundaries: publish the native progression and inventory after
// shop/office changes; apply only after UI has released its item pointers.
void house_shop_sync(bool force = false);
void house_shop_apply();
uint64_t house_shop_hash();
bool house_shop_client_active();
void house_shop_update(void* shop);
bool house_shop_buy(void* shop, void* item, HouseShopBuy buy);
bool house_shop_on_choice(const ChoiceMsg& msg, uint8_t from);
bool house_shop_on_message(const LogFileMsg& msg);
}
