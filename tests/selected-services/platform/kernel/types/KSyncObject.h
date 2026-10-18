#pragma once
#include "KObject.h"
namespace skyline::kernel::type {
class KSyncObject : public KObject {
public:
 bool signalled;
 KSyncObject(const DeviceState &state,KType type,bool flag=false):KObject(state,type),signalled(flag){}
 virtual ~KSyncObject()=default;
};
}
