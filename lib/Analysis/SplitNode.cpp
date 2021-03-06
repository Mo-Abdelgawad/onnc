//===- SplitNode.cpp ------------------------------------------------------===//
//
//                             The ONNC Project
//
// See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
#include <onnc/Analysis/SplitNode.h>
#include <onnc/IR/ONNXUtils.h>
#include <onnc/Support/IOStream.h>
#include <limits>
#include <iomanip> // for setw
#include <tuple>
#include <onnc/IR/Dump.h>

using namespace onnc;

typedef std::vector<xNode *> Nodes;
typedef std::unordered_set<xNode *> NodeSet;
typedef std::unordered_map<xNode *, unsigned> DegreeMap;

static bool IsType(const char *pKind, const xNode *pNode)
{
  return pNode->kind() == xSymbol(pKind);
}

static bool SkipWhenCalMemSize(const xNode *pNode)
{
  return IsType("Load", pNode) || IsType("Store", pNode) ||
         IsType("SubGraph", pNode);
}

static LongInts GetOutputValueSizes(const xNode& pN)
{
  if (pN.outputs().size())
    return GetValueSizes(*pN.outputs()[0]);
  return {};
}

//===----------------------------------------------------------------------===//
// SplitNode
//===----------------------------------------------------------------------===//
SplitNode::SplitNode(xNode& pN, bool pSizeDecideByOtherNode)
  : m_OutSizes(GetOutputValueSizes(pN)),
    m_SizeCalByOtherNode(pSizeDecideByOtherNode), m_Node(pN) {
  m_NewOutSizes = m_OutSizes;
}

bool SplitNode::useNewOutSize(const LongInts& pNewOutSize)
{
  m_NewOutSizes = pNewOutSize;
  return true;
}

LongInts SplitNode::calNewInputSize(unsigned pIdx) const
{
  return m_NewOutSizes;
}

LongInts SplitNode::getNewOutputSize(unsigned pIdx) const
{
  return m_NewOutSizes;
}

/// Factory
static SplitNode* SplitNodeCreator(xNode& pN);

//===----------------------------------------------------------------------===//
// Split Graph
//===----------------------------------------------------------------------===//
using NodeNodeMap = std::unordered_map<xNode*, xNode*>;

static void cloneNodeAndSuccessors(xNode *pNode, xGraph *pNewGraph,
                                   NodeNodeMap &pOldNewMap, NodeSet &pHasCloned)
{
  std::vector<xNode *> worklist;

  worklist.reserve(8);
  worklist.push_back(pNode);

  while (!worklist.empty()) {
    xNode *oldN = worklist.back();
    worklist.pop_back();
    if (pHasCloned.count(oldN))
      continue;

    pHasCloned.insert(oldN);

    xNode *newN = pNewGraph->create(oldN->kind(), oldN->outputs().size());
    newN->copyAttributes(*oldN);
    pNewGraph->appendNode(newN);
    pOldNewMap[oldN] = newN;

    for (unsigned i = 0; i < oldN->outputs().size(); ++i) {
      xValue *outv = oldN->outputs()[i];
      newN->outputs()[i]->copyMetadata(outv);

      for (auto u : outv->uses())
        worklist.push_back(u.user);
    }
  }
}

static void rebuildInputs(NodeNodeMap &pOldNewMap)
{
  // Rebuild inputs for newly created nodes from pOldNewMap.
  xNode *oldRetN = nullptr;
  for (auto &it : pOldNewMap) {
    xNode *oldN = it.first, *newN = it.second;
    for (xValue *oldv : oldN->inputs()) {
      auto valIt = pOldNewMap.find(oldv->node());
      if (valIt == pOldNewMap.end()) {
        outs() << "[Warning] rebuildInputs: required input value = "
               << oldv->uniqueName() << " is not found in new nodes map.\n";
        continue;
      }
      // [FIXME] We should remember newN's input[i] maps to which output[j] of
      //         newN's parent node.
      if (valIt->first->outputs().size() > 1) {
        outs() << "[Warning] rebuildInputs: parent node "
               << valIt->first->outputs()[0]->uniqueName()
               << " has more than one output value.\n";
      }

      // A graph has only one return node.
      if (newN->kind() == xBuiltinSymbol::kReturn)
        oldRetN = oldN;

      newN->addInput(valIt->second->outputs()[0]);
    }
  }

  if (oldRetN) {
    xNode *newRetN = pOldNewMap[oldRetN];
    xNode *graphRetN = newRetN->owningGraph()->return_node();
    for (xValue *input : newRetN->inputs())
      graphRetN->addInput(input);

    newRetN->destroy();
    pOldNewMap.erase(oldRetN);
  }
}

static void removeNodeAndSuccessors(xNode *pNode, NodeSet &pHasRemoved)
{
  std::vector<xNode *> worklist;

  worklist.reserve(8);
  worklist.push_back(pNode);

  while (!worklist.empty()) {
    xNode *oldN = worklist.back();
    worklist.pop_back();
    // Can't delete return node.
    if (oldN->kind() == xBuiltinSymbol::kReturn)
      continue;

    if (pHasRemoved.count(oldN))
      continue;

    for (xValue *outv : oldN->outputs())
      for (auto u : outv->uses()) {
        u.user->removeAllInputs();
        worklist.push_back(u.user);
      }

    oldN->destroy();
    pHasRemoved.insert(oldN);
  }
}

using LoadStorePair = std::pair<xNode*, xNode*>;

// [TODO] Please merge with NodeIRScheduler.cpp::InsertLoadStoreNode
static void createLoadStoreAtNode(xGraph &pGraph, xNode &pN,
                                  std::vector<LoadStorePair> &pNewLoadStores)
{
  // Create new store and load pairs.
  for (xValue *outv : pN.outputs()) {
    xNode* first = nullptr;
    for(auto u : outv->uses()) {
      if (!first) {
        first = u.user;
        continue;
      }

      if (!first->isBefore(u.user))
        first = u.user;
    }

    // Create load node and insert before the first use node.
    // FIXME: the first using should be in the same group, should we need to
    //        check this?
    xNode* loadN = pGraph.create(xSymbol("Load"));
    loadN->insertBefore(first);
    loadN->output()->copyMetadata(outv);
    outv->replaceAllUsesWith(loadN->output());

    // create store after creating load (since replaceAllUseWith).
    //
    // Note StoreNode is created with an output, the main reason is we want be
    // able to connect StoreNode to Subgraph node by setting StoreNode's output
    // to Subgraph's input.
    xNode* storeN = pGraph.create(xSymbol("Store"), {outv});
    storeN->output()->copyMetadata(outv);
    storeN->output()->setUniqueName(outv->uniqueName() + ".store");
    storeN->insertAfter(&pN);

    pNewLoadStores.emplace_back(loadN, storeN);
  }
}

static DegreeMap BuildDegreeMap(xGraph &pGraph);

static void TopologicalSort(xGraph &pGraph)
{
  DegreeMap dmap = BuildDegreeMap(pGraph);
  Nodes worklist;

  // Add degree = 0 to worklist in graph order.
  for (xNode *n : pGraph.nodes()) {
    if (n->kind() == xBuiltinSymbol::kUndefined)
      continue;

    if (dmap[n] == 0)
      worklist.push_back(n);
  }

  // topological sort.
  Nodes orderedList;
  while (!worklist.empty()) {
    xNode *n = worklist.back();
    worklist.pop_back();
    orderedList.push_back(n);
    for (xValue *v : n->outputs()) {
      // Update degree map.
      for(auto u : v->uses()) {
        if (u.user->kind() == xBuiltinSymbol::kReturn)
          continue;
        auto it = dmap.find(u.user);
        assert(it != dmap.end() &&
               "Node doesn't exist in dmap!?");
        // --Degree
        it->second -= 1;
        if (it->second == 0)
          worklist.push_back(it->first);
      } // for each user of this value.
    } // for each output value.
  }

  // Reorder the IR position based on topological sort.
  auto it = pGraph.begin();
  if (it->kind() == xBuiltinSymbol::kUndefined)
    ++it;

  for (unsigned i = 0; i < orderedList.size(); ++i) {
    xNode *n = orderedList[i];
    if (*it != n)
      n->moveBefore(*it);
    else
      ++it;
  }
}

static xGraph *SplitSubGraph(xGraph &pGraph, Nodes &pSplitPts)
{
  // Create new sub graph.
  // Note: 1. new sub graph does not include split points.
  //       2. new sub graph should be deleted by DeleteSubGraph pass.
  // TODO: DeleteSubGraph
  xGraph *newGraph = new xGraph();
  newGraph->setName(pGraph.name() + ".sub");

  std::vector<LoadStorePair> newLoadStores;

  // Create load/store to split graph.
  for (xNode *spNode : pSplitPts) {
    if (IsType("Load", spNode))
      newLoadStores.emplace_back(spNode, nullptr);
    else
      createLoadStoreAtNode(pGraph, *spNode, newLoadStores);
  }

  // <old node, new node>
  NodeNodeMap OldNewMap;

  // Now, clone loads and it's successors to new graph.
  NodeSet hasClonedSet;
  for (auto &ldst : newLoadStores)
    cloneNodeAndSuccessors(ldst.first, newGraph, OldNewMap, hasClonedSet);

  rebuildInputs(OldNewMap);

  // Create a new node to contain the subgraph
  xNode *subgN = pGraph.create(xSymbol("SubGraph"));
  subgN->g_(subgN->kind(), std::unique_ptr<xGraph>(newGraph));

  // remove load and it's successors from old graph, and connect store to the
  // new subgraph node.
  NodeSet hasRemovedSet;
  for (auto &ldst : newLoadStores) {
    removeNodeAndSuccessors(ldst.first, hasRemovedSet);
    if (ldst.second)
      subgN->addInput(ldst.second->output());
  }

  subgN->insertBefore(pGraph.return_node());
  pGraph.return_node()->addInput(subgN->output());

  TopologicalSort(*newGraph);

  return newGraph;
}

//===----------------------------------------------------------------------===//
// SplitGraph
//===----------------------------------------------------------------------===//
SplitGraph::SplitGraph(SplitGraphManager &pSgMgr, xGraph &pGraph)
  : m_SgMgr(pSgMgr), m_Graph(pGraph), m_AllocSuccess(false), m_AllocSize(0)
{
  rebuildSplitNodes();
}

void SplitGraph::rebuildSplitNodes()
{
  clear();
  for (xNode *n : m_Graph.nodes()) {
    if (n->kind() == xBuiltinSymbol::kUndefined ||
        IsType("SubGraphKind", n))
      continue;

    SplitNode *sn = SplitNodeCreator(*n);
    m_SplitNodes[n] = sn;

    if (IsType("Store", n)) {
      m_CurSplitAxis.push_back(0);
      m_CurSplitFactor.push_back(1);
      m_Stores.push_back(n);
    }
  }
}

SplitGraph::~SplitGraph()
{
  clear();
}

void SplitGraph::clear()
{
  m_Stores.clear();
  m_CurSplitAxis.clear();
  m_CurSplitFactor.clear();
  for (auto snIt : m_SplitNodes)
    delete snIt.second;
  m_SplitNodes.clear();
}

void SplitGraph::resetToOrigSize()
{
  for (auto it : m_SplitNodes)
    it.second->resetSize();

  for (unsigned i = 0; i < m_Stores.size(); ++i) {
    m_CurSplitAxis[i] = 0;
    m_CurSplitFactor[i] = 1;
  }
}

// TODO: getMemUsage: use a graph, and traverse graph
void SplitGraph::getMemUsage(ValMemSizeMap &pVMSMap) const
{
  for (const auto &snIt: m_SplitNodes) {
    const xNode *n = snIt.first;
    const SplitNode *sn = snIt.second;

    // user node will calculate its memory size.
    if (sn->skipWhenCalMemSize())
      continue;

    // get required memory size of each input.
    for (unsigned i = 0; i < n->inputs().size(); ++i) {
      const xValue *v = n->inputs()[i];
      pVMSMap[v] = m_SgMgr.getTTI().getOperatorInputMemUsage(n, i,
                                                        sn->calNewInputSize(i));
    }

    // get required memory size of each output.
    for (unsigned i = 0; i < n->outputs().size(); ++i) {
      const xValue *v = n->outputs()[i];
      pVMSMap[v] = m_SgMgr.getTTI().getOperatorOutputMemUsage(n, i,
                                                        sn->calNewInputSize(i));
    }
  }
}

void SplitGraph::shrinkSize()
{
  // Shrink size, and propogate backward.
  for (unsigned i = 0; i < m_Stores.size(); ++i) {
    xNode *n = m_Stores[i];

    const TensorSizes &origSizes = n->inputs()[0]->sizes();
    unsigned &factor = m_CurSplitFactor[i],
             &splitAxis = m_CurSplitAxis[i];
    ++factor;
    // Can't divide this axis further, try next axis.
    if (origSizes[splitAxis].dim < factor) {
      ++splitAxis;
      factor = 1;
    }

    // Unable divide further, give up shrink this output value.
    if (origSizes.size() == splitAxis)
      continue;

    splitNodeByFactor(n, splitAxis, factor, true);
  }
}

SplitNode* SplitGraph::getSplitNode(xNode* pN)
{
  assert(m_SplitNodes.find(pN) != m_SplitNodes.end() &&
         "xNode doesn't exist in SplitGraph.");
  return m_SplitNodes[pN];
}

const SplitNode* SplitGraph::getSplitNode(xNode* pN) const
{
  auto it = m_SplitNodes.find(pN);
  assert(m_SplitNodes.find(pN) != m_SplitNodes.end() &&
         "xNode doesn't exist in SplitGraph.");
  return it->second;
}

void SplitGraph::splitNodeByFactor(xNode* pN, unsigned pAxis,
                                   unsigned pFactor, bool pUpdateUpper)
{
  SplitNode* ns = getSplitNode(pN);
  // FIXME: If the node has multiple outputs?
  LongInts newS = ns->getNewOutputSize(0);
  newS[pAxis] = (newS[pAxis] + pFactor - 1)/ pFactor;
  splitNodeBySize(pN, newS, pUpdateUpper);
}

bool SplitGraph::hasSplitNode(xNode *pN) const
{
  return m_SplitNodes.find(pN) != m_SplitNodes.end();
}

bool SplitGraph::splitNodeBySize(xNode* pN,
                                       const LongInts& pNewOutSize,
                                       bool pUpdateUpper)
{
  SplitNode* ns = getSplitNode(pN);
  if (!ns->useNewOutSize(pNewOutSize))
    return false;

  // Load IR is a boundary, it is paired with Store IR and form a subgraph.
  if (!pUpdateUpper)
    return true;

  bool status = true;
  for (unsigned i = 0; i < ns->getNode().inputs().size(); ++i) {
    if (xNode* child = ns->getNode().inputs()[i]->node()) {
      if (child->kind() == xBuiltinSymbol::kParam)
        continue;
      LongInts newInS = ns->calNewInputSize(i);
      status &= splitNodeBySize(child, newInS, true);
    }
  }
  return status;
}

// [TODO] BuildDegreeMap: Duplicate code, please merge with NodeIRScheduler
static DegreeMap BuildDegreeMap(xGraph &pGraph)
{
  DegreeMap dmap;
  for (xNode *n : pGraph.nodes()) {
    if (n->kind() == xBuiltinSymbol::kUndefined)
      continue;

    unsigned degcnt = n->inputs().size();
    for (xValue *v : n->inputs())
      if (v->node() == nullptr) {
        outs() << "Warning! " << n->kind().toString()
               << " use a value = " << v->uniqueName()
               << ", which doen't bind to a node";
        --degcnt;
      }
    dmap[n] = degcnt;
  }
  return dmap;
}

Nodes findHalfSizeSplitPoints(xGraph &pGraph,
                              const TargetTransformInfo &pTTI)
{
  // get total required size.
  uint64_t total = 0;
  std::unordered_map<xNode *, uint64_t> nodeSize;

  for (xNode *n : pGraph.nodes()) {
    // skip load and store since they are calculated by their user nodes.
    if (n->kind() == xBuiltinSymbol::kUndefined ||
        SkipWhenCalMemSize(n))
      continue;

    nodeSize[n] = pTTI.getOperatorMemUsage(n).size;
    total += nodeSize[n];
  }

  // Build degree map and do topological + dfs traversing.
  DegreeMap dmap = BuildDegreeMap(pGraph);
  Nodes worklist;

  // Add degree = 0 to worklist in graph order.
  for (xNode *n : pGraph.nodes()) {
    if (n->kind() == xBuiltinSymbol::kUndefined)
      continue;

    if (dmap[n] == 0)
      worklist.push_back(n);
  }

  std::unordered_set<xNode *> grpA, grpB;
  uint64_t accumulateSize = 0;
  uint64_t size_a = 0;

  // topological + dfs traversing
  xNode *lastNode = nullptr;
  while (!worklist.empty()) {
    xNode *n = worklist.back();
    worklist.pop_back();
    for (xValue *v : n->outputs()) {
      // Update degree map.
      for(auto u : v->uses()) {
        if (u.user->kind() == xBuiltinSymbol::kReturn)
          continue;
        auto it = dmap.find(u.user);
        assert(it != dmap.end() &&
               "Node doesn't exist in dmap!?");
        // --Degree
        it->second -= 1;
        if (it->second == 0)
          worklist.push_back(it->first);
      } // for each user of this value.
    } // for each output value.

    if (SkipWhenCalMemSize(n))
      continue;

    if (accumulateSize < total/2) {
      grpA.insert(n);
      size_a = accumulateSize;
    } else {
      grpB.insert(n);
    }

    lastNode = n;
    accumulateSize += pTTI.getOperatorMemUsage(n).size;
  }

  if (grpB.empty()) {
    // Can not split further.
    if (grpA.size() == 1)
      return {};
    grpB.insert(lastNode);
    grpA.erase(lastNode);
  }

  // put load/store to correct group.
  for (xNode *n : pGraph.nodes()) {
    if (IsType("Load", n)) {
      // Assume users of this value are in the same group.
      xNode *user = n->output()->uses()[0].user;
      if (grpA.count(user))
        grpA.insert(n);
      else
        grpB.insert(n);

    } else if (IsType("Store", n) ||
               IsType("SubGraph", n)) {
      if (grpA.count(n->input()->node()))
        grpA.insert(n);
      else
        grpB.insert(n);
    }
  }

  // find split points from group A.
  Nodes splitPts;
  for (xNode *n : grpA) {
    // If the node's user is not in grpA, add to split points.
    for (xValue *outv : n->outputs())
      for (auto u : outv->uses())
        if (!grpA.count(u.user)) {
          splitPts.push_back(n);
          assert(grpB.count(u.user) && "Node is not in both split group!?");
        }
  }

  // Add all load of grpB to splitPts
  for (xNode *n : grpB)
    if (IsType("Load", n))
      splitPts.push_back(n);

  return splitPts;
}

void SplitGraph::setAllocStatus(bool success, uint64_t size)
{
  m_AllocSuccess = success;
  m_AllocSize = size;
}

//===----------------------------------------------------------------------===//
// SplitGraphManager
//===----------------------------------------------------------------------===//
SplitGraphManager::SplitGraphManager(xGraph& pGraph,
                                   DLATargetBackend& pDLATB)
  : m_DLATB(pDLATB)
{
  m_SubGraphs.push_back(new SplitGraph(*this, pGraph));
}

SplitGraphManager::~SplitGraphManager()
{
  clear();
}

void SplitGraphManager::clear()
{
  for (SplitGraph *sg : m_SubGraphs)
    delete sg;
  m_SubGraphs.clear();
}

SplitGraph *SplitGraphManager::splitNewSubGraph(SplitGraph &pSpGraph)
{
  Nodes splitPts = findHalfSizeSplitPoints(pSpGraph.getGraph(),
                                           *m_DLATB.getTTI());
  // Failed splitting.
  if (splitPts.empty())
    return nullptr;

  xGraph *newGraph = SplitSubGraph(pSpGraph.getGraph(), splitPts);

  // rebuild m_SplitNodes for original SplitGraph
  pSpGraph.rebuildSplitNodes();

  m_SubGraphs.push_back(new SplitGraph(*this, *newGraph));
  return m_SubGraphs.back();
}

void SplitGraphManager::dump() const
{
  print(outs());
}

void PrintAttr(OStream &pOS, const xNode &pN)
{
  for (auto attrId : pN.attributeNames()) {
    pOS << attrId.toString() << ": ";
    switch (pN.kindOf(attrId)) {
    case xAttributeKind::f   :
      pOS << pN.f(attrId) << " ";
      break;
    case xAttributeKind::fs  :
      for (float f : pN.fs(attrId))
        pOS << f << " ";
      break;
    case xAttributeKind::i   :
      pOS << pN.i(attrId) << " ";
      break;
    case xAttributeKind::is  :
      for (int i : pN.is(attrId))
        pOS << i << " ";
      break;
    case xAttributeKind::s   :
      pOS << pN.s(attrId) << " ";
      break;
    case xAttributeKind::ss  :
      for (auto &s : pN.ss(attrId))
        pOS << s << " ";
      break;
    default :
      pOS << "[TODO]";
    }
    pOS << " ";
  }
}

void SplitGraph::print(OStream &pOS) const
{
  size_t graphOldS = 0, graphNewS = 0;
  pOS << "Graph = " << m_Graph.name() << " " << &m_Graph << "\n"
      << "  allocation status = " << (m_AllocSuccess ? "success" : "failed")
      << " with size " << m_AllocSize << "\n";
  for (const xNode *n : m_Graph.nodes()) {
    if (n->kind() == xBuiltinSymbol::kUndefined)
      continue;

    if (IsType("SubGraph", n)) {
      PrintNode(pOS, *const_cast<xNode *>(n));
      continue;
    }

    std::vector<LongInts> newInputSizes;
    const SplitNode *sn = getSplitNode(const_cast<xNode *>(n));
    pOS << n->kind().toString() << ": ";
    PrintAttr(pOS, *n);
    pOS << "\n";

    pOS << "  inputs:\n";
    for (int i = 0; i < n->inputs().size(); ++i) {
      LongInts newInS = sn->calNewInputSize(i);
      newInputSizes.push_back(newInS);

      const xValue *v = n->inputs()[i];

      pOS << "    " << std::setw(12) << std::left << v->uniqueName() << "(";
      for (auto d : v->sizes())
        pOS << std::setw(5) << std::right << d.dim;

      pOS << ") -> (";
      for (int64_t s : newInS)
        pOS << std::setw(5) << std::right << s;
      pOS << ")\n";
    }

    pOS << "  outputs:\n";
    for (int i = 0; i < n->outputs().size(); ++i) {
      const xValue *v = n->outputs()[i];

      pOS << "    " << std::setw(12) << std::left << v->uniqueName() << "(";
      for (auto s : v->sizes())
        pOS << std::setw(5) << std::right << s.dim;

      pOS << ") -> (";
      for (int64_t s : sn->getNewOutputSize(i))
        pOS << std::setw(5) << std::right << s;
      pOS << ")\n";
    }

    // don't count store/load node since it's size has been added by upper node.
    if (sn->skipWhenCalMemSize())
      continue;

    MemSize newS =
      m_SgMgr.getTTI().getOperatorMemUsage(n, newInputSizes,
                                           {sn->getNewOutputSize(0)});
    MemSize oldS = m_SgMgr.getTTI().getOperatorMemUsage(n);

    graphOldS += oldS.size;
    graphNewS += newS.size;
    pOS << "  total: " << oldS.size/1024.f << " kb -> "
        << newS.size/1024.f << " kb" << "\n";
  }
  pOS << "Graph total size: " << (float)graphOldS/1024.f << " kb -> "
      << (float)graphNewS/1024.f << " kb" << "\n";
}

void SplitGraphManager::print(OStream &pOS) const
{
  for (SplitGraph *spGraph : m_SubGraphs) {
    pOS << "Print graph allocation info:\n";
    spGraph->print(pOS);
  }
}

//===----------------------------------------------------------------------===//
// Derived of SplitNode
//===----------------------------------------------------------------------===//
class SplitConv : public SplitNode {
public:
  SplitConv(xNode& pN)
    : SplitNode(pN) {
    assert(pN.kind() == xBuiltinSymbol::kConv && "This is not a convolution node.");

    GetConvKernelShape(pN, m_KShape);
    GetAttrVals(pN, xBuiltinSymbol::kstrides, m_Stride);
    GetPads(pN, m_PadBegin, m_PadEnd);
  }

  LongInts calNewInputSize(unsigned pIdx) const override
  {
    // Conv inputs:
    //  0   x:T   (N x C x D1 x D2 .. Dn)
    //  1   w:T   (M x C x k1 x k2 .. kn)
    //  2   B:T   (M)
    //
    //  kernel_shape  [k1 x k2 .. kn]
    //  pads          [x1_begin, x2_begin .. x1_end, x2_end]
    //  strides       [s1 x s2 .. sn]
    //
    // Conv output:
    //  0   y:T   (N x M x [(D1 - K1 + x1_begin + x1_end)/s1 + 1] x ..)
    //
    // Please also ref: UpdateGraphOutputSize.cpp:UpdateConvOutputInfo.
    switch (pIdx) {
    case 0: {
      LongInts newIS(4); // common case: N C H W
      const TensorSizes &xDim = m_Node.inputs()[0]->sizes();
      newIS[0] = m_NewOutSizes[0];
      newIS[1] = xDim[1].dim;
      const size_t numAxis = xDim.size() - 2;
      for (int i = 0; i < numAxis; ++i) {
        newIS[i + 2] = (m_NewOutSizes[i + 2] - 1) * m_Stride[i]
                       - m_PadBegin[i] - m_PadEnd[i] + m_KShape[i];
      }
      return newIS;
    }
    case 1: {
      const TensorSizes &wDim = m_Node.inputs()[1]->sizes();
      LongInts newIS(wDim.size());
      newIS[0] = m_NewOutSizes[1];
      for (int i = 1; i < wDim.size(); ++i)
        newIS[i] = wDim[i].dim;
      return newIS;
    }
    case 2:
      return {m_NewOutSizes[1]};
    default:
      assert(false && "Error in SplitConv::calNewInputSize. Invalid input id.");
      return {};
    }
  }

private:
  LongInts m_PadBegin, m_PadEnd;
  LongInts m_KShape;
  LongInts m_Stride;
};

class SplitGemm : public SplitNode {
public:
  SplitGemm(xNode& pN)
    : SplitNode(pN) {
  }

  LongInts calNewInputSize(unsigned pIdx) const override
  {
    // Gemm inputs:
    //  0   A:T   (M x K)
    //  1   B:T   (K x N)
    //  2   C:T   (M x N)
    //
    //  broadcast  [int]
    //  transA     [int]
    //  transB     [int]
    //
    // Gemm output:
    //  0   Y:T   (M x N)
    const TensorSizes &aDim = m_Node.inputs()[0]->sizes();
    const int64_t K = IsTranspose(m_Node, xBuiltinSymbol::ktransA) ?
                        aDim[0].dim : aDim[1].dim;

    switch (pIdx) {
    case 0: {
      // Get new size of A.
      if (IsTranspose(m_Node, xBuiltinSymbol::ktransA))
        return {K, m_NewOutSizes[0]};
      return {m_NewOutSizes[0], K};
    }
    case 1: {
      // Get new size of B.
      if (IsTranspose(m_Node, xBuiltinSymbol::ktransB))
        return {m_NewOutSizes[1], K};
      return {K, m_NewOutSizes[1]};
    }
    case 2: {
      // [FIXME] We use original value. Should we change this?
      return GetValueSizes(*m_Node.inputs()[2]);
    }
    default:
      assert(false && "Error in SplitGemm::calNewInputSize. Invalid input id.");
      return {};
    }
  }
};

class SplitPool : public SplitNode {
public:
  SplitPool(xNode& pN)
    : SplitNode(pN) {
    assert(pN.kind() == xSymbol("MaxPool") && "This is not a pool node.");

    GetAttrVals(pN, xBuiltinSymbol::kkernel_shape, m_KShape);
    GetAttrVals(pN, xBuiltinSymbol::kstrides, m_Stride);
    GetPads(pN, m_PadBegin, m_PadEnd);
  }

  LongInts calNewInputSize(unsigned pIdx) const override
  {
    assert(pIdx == 0 && "SplitPool::calNewInputSize: Invalid input id.");

    LongInts newIS(4); // common case: N C H W
    const TensorSizes &xDim = m_Node.inputs()[0]->sizes();
    newIS[0] = m_NewOutSizes[0];
    newIS[1] = m_NewOutSizes[1];
    const size_t numAxis = xDim.size() - 2;
    for (int i = 0; i < numAxis; ++i) {
      newIS[i + 2] = m_NewOutSizes[i + 2] * m_Stride[i]
                     - m_PadBegin[i] - m_PadEnd[i] + 2 * (m_KShape[i] / 2);
    }
    return newIS;
  }

private:
  LongInts m_PadBegin, m_PadEnd;
  LongInts m_KShape;
  LongInts m_Stride;
};

class SplitReshape : public SplitNode {
public:
  SplitReshape(xNode& pN)
    : SplitNode(pN) {
  }

  LongInts calNewInputSize(unsigned pIdx) const override
  {
    assert(pIdx <= 1 && "SplitReshape::calNewInputSize: Invalid input id.");

    if (pIdx == 1)
      return {};

    // [FIXME] It is hard to compute new input size for reshape. Here has some
    //         assumptions and is buggy.
    // [TODO] If there is un-handled case, just give up.
    //
    // Assume we are handling this kind of case:
    // Reshape
    //   pool5_1     : size = 4 (   10  256    6    6)
    //   OC2_DUMMY_1 : size = 1 (    2)
    //   OC2_DUMMY_0 : size = 2 (   10 9216)
    // Gemm
    //   OC2_DUMMY_0 : size = 2 (   10 9216)
    //   fc6_w_0     : size = 2 ( 4096 9216)
    //   fc6_b_0     : size = 1 ( 4096)
    //   fc6_1       : size = 2 (   10 4096)
    assert(m_OutSizes.size() == 2 && "Reshape size assumption.");

    xValue *v = m_Node.inputs()[0];
    const TensorSizes &origSizes = v->sizes();
    LongInts newIS(origSizes.size());

    newIS[0] = m_NewOutSizes[0];

    int64_t origCHWSize = 1, newCHWSize = 1;
    for (int i = 1; i < m_NewOutSizes.size(); ++i) {
      origCHWSize *= m_NewOutSizes[i];
      newCHWSize *= m_OutSizes[i];
    }

    assert(origCHWSize >= newCHWSize && "SplitReshape: Invalid resize.");

    if (origCHWSize % newCHWSize != 0) {
      errs() << "SplitReshape: origCHWSize mod newCHWSize is not zero!\n"
             << "  " << origCHWSize << " % " << newCHWSize << "\n";
    }

    int64_t resizeFactor = origCHWSize / newCHWSize;

    if (origSizes[1].dim % resizeFactor != 0) {
      errs() << "SplitReshape: origSizes[1].dim mod resizeFactor is not zero!\n"
             << "  " << origSizes[1].dim << " % " << resizeFactor << "\n";
    }

    newIS[1] = origSizes[1].dim / resizeFactor;
    newIS[2] = origSizes[2].dim;
    newIS[3] = origSizes[3].dim;
    return newIS;
  }
};

static SplitNode* SplitNodeCreator(xNode& pN)
{
  if (OutputSizeIsInputSize(pN))
    return new SplitNode(pN);

  // load output size and store input size is calculated on its successor and
  // predecessor node.
  if (IsType("Load", &pN) ||
      IsType("Store", &pN))
    return new SplitNode(pN, true);

  const auto kind = pN.kind();
  if (kind == xBuiltinSymbol::kConv) {
    return new SplitConv(pN);
  } else if (kind == xSymbol("MaxPool")) {
    return new SplitPool(pN);
  } else if (kind == xBuiltinSymbol::kGemm) {
    return new SplitGemm(pN);
  } else if (kind == xBuiltinSymbol::kReshape) {
    return new SplitReshape(pN);
  }

  errs() << "Unsupport node: " << kind.toString() << "\n";
  assert(false && "Unsupport node.");
  return nullptr;
}
