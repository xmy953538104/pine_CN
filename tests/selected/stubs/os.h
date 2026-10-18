#pragma once
#include <common.h>
#include <crypto/key_store.h>
#include <loader/loader.h>
namespace skyline::catalog {struct Catalog;}
namespace skyline::loader {struct SelectionRequest;}
namespace skyline::kernel {class OS{public:std::shared_ptr<crypto::KeyStore>keyStore;DeviceState state;OS(){state.os=this;}
void ExecuteSelectedContent(const catalog::Catalog&,const loader::SelectionRequest&,std::shared_ptr<crypto::KeyStore>);
private:void ExecuteLoadedContent(const char *metadataSource="base_control");};}
