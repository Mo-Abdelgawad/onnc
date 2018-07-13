//===- ATenLower.cpp -------------------------------------------------------===//
//
//                             The ONNC Project
//
// See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
#include <onnc/Transforms/TensorSel/Lower.h>
#include <onnc/Transforms/TensorSel/Standards/ATenLower.h>
#include <onnc/IR/Compute/ATen.h>

using namespace onnc;

//===----------------------------------------------------------------------===//
// ATenLower
//===----------------------------------------------------------------------===//
ATenLower::ATenLower()
{
}

ATenLower::~ATenLower()
{
}

int ATenLower::isMe(const ::onnx::Node& pNode) const
{
  if (pNode.kind() == ::onnx::Symbol("ATen"))
    return kStdLower;
  return kNotMe;
}

ComputeOperator*
ATenLower::activate(ComputeGraph& pGraph, ::onnx::Node& pNode) const
{
  // check input/output name
  if (pNode.inputs().empty())
    return nullptr;

  for (::onnx::Value* xv : pNode.inputs()) {
    if (!xv->has_unique_name())
      return nullptr;
  }

  if (pNode.outputs().empty())
    return nullptr;

  for (::onnx::Value* xv : pNode.outputs()) {
    if (!xv->has_unique_name())
      return nullptr;
  }

  // create operators
  onnc::ATen* op = pGraph.addOperator<onnc::ATen>();

  // set input/output
  for (::onnx::Value* xv : pNode.inputs())
    op->addInput(*pGraph.getValue<onnc::Tensor>(xv->uniqueName()));

  for (::onnx::Value* xv : pNode.outputs())
    op->addOutput(*pGraph.getValue<onnc::Tensor>(xv->uniqueName()));

  return op;
}
