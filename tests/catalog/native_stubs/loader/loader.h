#pragma once
#include <common.h>
namespace skyline::loader {
enum class LoaderResult : int8_t {
    Success, ParsingError, MissingHeaderKey, MissingTitleKey, MissingTitleKek,
    MissingKeyArea, ErrorSparseNCA, ErrorCompressedNCA,
};
class loader_exception : public exception {
  public:
    const LoaderResult error;
    explicit loader_exception(LoaderResult pError) : exception("controlled loader error"), error(pError) {}
};
}
