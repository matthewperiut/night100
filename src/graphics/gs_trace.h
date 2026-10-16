#pragma once
#include "runtime/gs/gs_backend.h"
#include <memory>
#include <string>
namespace GSTrace {
// Local diagnostic ABI, versioned and size-checked. No disc data is vendored.
enum class Op : uint32_t { End=0, Initialize, Reset, Submit, Clut, Transfer, Upload, Flush, TextureFlush, Sync, Present, Clear, Consume, Write };
struct Header { char magic[8];uint32_t version,batchSize,contextSize,presentationSize; };
struct Record { Op op;uint32_t size; };
struct Clut {GSTex0Reg tex;GSTexClutReg clut;};
struct Clear {GSContext context;uint32_t rgba;};
struct Write {uint32_t psm,base,bw,x,y,value;};
std::unique_ptr<GSRasterBackend> record(std::unique_ptr<GSRasterBackend>,const std::string& path);
}
