#pragma once
#include <common.h>
#include <vfs/npdm.h>
#include "KObject.h"
#include "KThread.h"
namespace skyline::kernel::type {
class KProcess {
 const DeviceState &state;
 std::map<KHandle,std::shared_ptr<KObject>> handles;
 KHandle next{1};
public:
 explicit KProcess(const DeviceState &state):state(state){}
 vfs::NPDM npdm;
 template<class T> struct HandleOut {std::shared_ptr<T> item;KHandle handle;};
 template<class T,class... Args> HandleOut<T> NewHandle(Args&&...args){auto item=std::make_shared<T>(state,std::forward<Args>(args)...);auto h=InsertItem(item);return{item,h};}
 template<class T> KHandle InsertItem(std::shared_ptr<T>item){auto h=next++;handles.emplace(h,std::move(item));return h;}
 template<class T> std::shared_ptr<T> GetHandle(KHandle h){return std::static_pointer_cast<T>(handles.at(h));}
 size_t HandleCount() const {return handles.size();}
};
}
