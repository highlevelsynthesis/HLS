// (C) Copyright 2016-2020 Xilinx, Inc.
// All Rights Reserved.
//
// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.
//===----------------------------------------------------------------------===//
//
// This file defines a simple interface that can be used to print out generic
// LLVM graphs to ".dot" files.  "dot" is a tool that is part of the AT&T
// graphviz package (http://www.research.att.com/sw/tools/graphviz/) which can
// be used to turn the files output by this interface into a variety of
// different graphics formats.
//
// Graphs do not need to implement any interface past what is already required
// by the GraphTraits template, but they can choose to implement specializations
// of the DOTGraphTraits template if they want to customize the graphs output in
// any way.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_SUPPORT_GRAPHWRITERHLS_H
#define LLVM_SUPPORT_GRAPHWRITERHLS_H

#include "llvm/ADT/GraphTraits.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/Twine.h"
#include "llvm/Support/DOTGraphTraits.h"
#include "llvm/Support/raw_ostream.h"
#include <algorithm>
#include <cstddef>
#include <iterator>
#include <string>
#include <type_traits>
#include <vector>

namespace llvm {

namespace DOT {  // Private functions...

std::string EscapeString(const std::string &Label);

/// \brief Get a color string for this node number. Simply round-robin selects
/// from a reasonable number of colors.
StringRef getColorString(unsigned NodeNumber);

} // end namespace DOT

#if 0
namespace GraphProgram {

enum Name {
  DOT,
  FDP,
  NEATO,
  TWOPI,
  CIRCO
};

} // end namespace GraphProgram

bool DisplayGraph(StringRef Filename, bool wait = true,
                  GraphProgram::Name program = GraphProgram::DOT);
#endif

template<typename GraphType>
class GraphWriterHLS {
  raw_ostream &O;
  const GraphType &G;

  using DOTTraits = DOTGraphTraits<GraphType>;
  using GTraits = GraphTraits<GraphType>;
  using NodeRef = typename GTraits::NodeRef;
  using node_iterator = typename GTraits::nodes_iterator;
  using child_iterator = typename GTraits::ChildIteratorType;
  DOTTraits DTraits;

  static_assert(std::is_pointer<NodeRef>::value,
                "FIXME: Currently GraphWriterHLS requires the NodeRef type to be "
                "a pointer.\nThe pointer usage should be moved to "
                "DOTGraphTraits, and removed from GraphWriterHLS itself.");

  // Writes the edge labels of the node to O and returns true if there are any
  // edge labels not equal to the empty string "".
  bool getEdgeSourceLabels(raw_ostream &O, NodeRef Node) {
    child_iterator EI = GTraits::child_begin(Node);
    child_iterator EE = GTraits::child_end(Node);
    bool hasEdgeSourceLabels = false;

    for (unsigned i = 0; EI != EE && i != 64; ++EI, ++i) {
      std::string label = DTraits.getEdgeSourceLabel(Node, EI);

      if (label.empty())
        continue;

      hasEdgeSourceLabels = true;

      if (i)
        O << "|";

      O << "<s" << i << ">" << DOT::EscapeString(label);
    }

    if (EI != EE && hasEdgeSourceLabels)
      O << "|<s64>truncated...";

    return hasEdgeSourceLabels;
  }

public:
  GraphWriterHLS(raw_ostream &o, const GraphType &g, bool SN) : O(o), G(g) {
    DTraits = DOTTraits(SN);
  }

  void writeGraph(const std::string &Title = "") {
    // Output the header for the graph...
    writeHeader(Title);

    // Emit all of the nodes in the graph...
    writeNodes();

    // Output any customizations on the graph
    DOTGraphTraits<GraphType>::addCustomGraphFeatures(G, *this);

    // Output the end of the graph
    writeFooter();
  }

  void writeHeader(const std::string &Title) {
    std::string GraphName = DTraits.getGraphName(G);

    if (!Title.empty())
      O << "digraph \"" << DOT::EscapeString(Title) << "\" {\n";
    else if (!GraphName.empty())
      O << "digraph \"" << DOT::EscapeString(GraphName) << "\" {\n";
    else
      O << "digraph unnamed {\n";

    if (DTraits.renderGraphFromBottomUp())
      O << "\trankdir=\"BT\";\n";

    if (!Title.empty())
      O << "\tlabel=\"" << DOT::EscapeString(Title) << "\";\n";
    else if (!GraphName.empty())
      O << "\tlabel=\"" << DOT::EscapeString(GraphName) << "\";\n";
    O << DTraits.getGraphProperties(G);
    O << "\n";
  }

  void writeFooter() {
    // Finish off the graph
    O << "}\n";
  }

  void writeNodes() {
    // Loop over the graph, printing it out...
    for (const auto Node : nodes<GraphType>(G))
      if (!isNodeHidden(Node))
        writeNode(Node);
  }

  bool isNodeHidden(NodeRef Node) {
    return DTraits.isNodeHidden(Node);
  }

  void writeNode(NodeRef Node) {
    std::string NodeAttributes = DTraits.getNodeAttributes(Node, G);

    O << "\tNode" << static_cast<const void*>(Node) << " [shape=record,";
    if (!NodeAttributes.empty()) O << NodeAttributes << ",";
    O << "label=\"{";

    if (!DTraits.renderGraphFromBottomUp()) {
      O << DOT::EscapeString(DTraits.getNodeLabel(Node, G));

      // If we should include the address of the node in the label, do so now.
      std::string Id = DTraits.getNodeIdentifierLabel(Node, G);
      if (!Id.empty())
        O << "|" << DOT::EscapeString(Id);

      std::string NodeDesc = DTraits.getNodeDescription(Node, G);
      if (!NodeDesc.empty())
        O << "|" << DOT::EscapeString(NodeDesc);
    }

    std::string edgeSourceLabels;
    raw_string_ostream EdgeSourceLabels(edgeSourceLabels);
    bool hasEdgeSourceLabels = getEdgeSourceLabels(EdgeSourceLabels, Node);

    if (hasEdgeSourceLabels) {
      if (!DTraits.renderGraphFromBottomUp()) O << "|";

      O << "{" << EdgeSourceLabels.str() << "}";

      if (DTraits.renderGraphFromBottomUp()) O << "|";
    }

    if (DTraits.renderGraphFromBottomUp()) {
      O << DOT::EscapeString(DTraits.getNodeLabel(Node, G));

      // If we should include the address of the node in the label, do so now.
      std::string Id = DTraits.getNodeIdentifierLabel(Node, G);
      if (!Id.empty())
        O << "|" << DOT::EscapeString(Id);

      std::string NodeDesc = DTraits.getNodeDescription(Node, G);
      if (!NodeDesc.empty())
        O << "|" << DOT::EscapeString(NodeDesc);
    }

    if (DTraits.hasEdgeDestLabels()) {
      O << "|{";

      unsigned i = 0, e = DTraits.numEdgeDestLabels(Node);
      for (; i != e && i != 64; ++i) {
        if (i) O << "|";
        O << "<d" << i << ">"
          << DOT::EscapeString(DTraits.getEdgeDestLabel(Node, i));
      }

      if (i != e)
        O << "|<d64>truncated...";
      O << "}";
    }

    O << "}\"";

    int FID = DTraits.getNodeFunctionID(Node, G);
    if(FID > 0)
      O << " fid=\"" << FID << "\"";

    O << " demanglename=\"" << DOT::EscapeString(DTraits.getNodeDemangleName(Node, G)) << "\"";
    O << " manglename=\"" << DOT::EscapeString(DTraits.getNodeMangleName(Node, G)) << "\"";
    std::string FileName = DTraits.getNodeFileName(Node, G);
    if(!FileName.empty())
      O << " filename=\"" << FileName << "\"";
    int Line = DTraits.getNodeLineNumber(Node, G);
    if(Line > 0)
      O << " linenumber=\"" << Line << "\"";

    O << "];\n";   // Finish printing the "node" line

    // Output all of the edges now
    child_iterator EI = GTraits::child_begin(Node);
    child_iterator EE = GTraits::child_end(Node);
    for (unsigned i = 0; EI != EE && i != 64; ++EI, ++i)
      if (!DTraits.isNodeHidden(*EI))
        writeEdge(Node, i, EI);
    for (; EI != EE; ++EI)
      if (!DTraits.isNodeHidden(*EI))
        writeEdge(Node, 64, EI);
  }

  void writeEdge(NodeRef Node, unsigned edgeidx, child_iterator EI) {
    if (NodeRef TargetNode = *EI) {
      int DestPort = -1;
      if (DTraits.edgeTargetsEdgeSource(Node, EI)) {
        child_iterator TargetIt = DTraits.getEdgeTarget(Node, EI);

        // Figure out which edge this targets...
        unsigned Offset =
          (unsigned)std::distance(GTraits::child_begin(TargetNode), TargetIt);
        DestPort = static_cast<int>(Offset);
      }

      if (DTraits.getEdgeSourceLabel(Node, EI).empty())
        edgeidx = -1;

      emitEdge(static_cast<const void*>(Node), edgeidx,
               static_cast<const void*>(TargetNode), DestPort,
               DTraits.getEdgeAttributes(Node, EI, G));
    }
  }

  /// emitSimpleNode - Outputs a simple (non-record) node
  void emitSimpleNode(const void *ID, const std::string &Attr,
                   const std::string &Label, unsigned NumEdgeSources = 0,
                   const std::vector<std::string> *EdgeSourceLabels = nullptr) {
    O << "\tNode" << ID << "[ ";
    if (!Attr.empty())
      O << Attr << ",";
    O << " label =\"";
    if (NumEdgeSources) O << "{";
    O << DOT::EscapeString(Label);
    if (NumEdgeSources) {
      O << "|{";

      for (unsigned i = 0; i != NumEdgeSources; ++i) {
        if (i) O << "|";
        O << "<s" << i << ">";
        if (EdgeSourceLabels) O << DOT::EscapeString((*EdgeSourceLabels)[i]);
      }
      O << "}}";
    }
    O << "\"];\n";
  }

  /// emitEdge - Output an edge from a simple node into the graph...
  void emitEdge(const void *SrcNodeID, int SrcNodePort,
                const void *DestNodeID, int DestNodePort,
                const std::string &Attrs) {
    if (SrcNodePort  > 64) return;             // Eminating from truncated part?
    if (DestNodePort > 64) DestNodePort = 64;  // Targeting the truncated part?

    O << "\tNode" << SrcNodeID;
    if (SrcNodePort >= 0)
      O << ":s" << SrcNodePort;
    O << " -> Node" << DestNodeID;
    if (DestNodePort >= 0 && DTraits.hasEdgeDestLabels())
      O << ":d" << DestNodePort;

    if (!Attrs.empty())
      O << "[" << Attrs << "]";
    O << ";\n";
  }

  /// getOStream - Get the raw output stream into the graph file. Useful to
  /// write fancy things using addCustomGraphFeatures().
  raw_ostream &getOStream() {
    return O;
  }
};

template<typename GraphType>
raw_ostream &WriteGraphHLS(raw_ostream &O, const GraphType &G,
                        bool ShortNames = false,
                        const Twine &Title = "") {
  // Start the graph emission process...
  GraphWriterHLS<GraphType> W(O, G, ShortNames);
  // Emit the graph.
  W.writeGraph(Title.str());

  return O;
}

std::string createGraphFilename(const Twine &Name, int &FD);

template <typename GraphType>
std::string WriteGraphHLS(const GraphType &G, const Twine &Name,
                       bool ShortNames = false, const Twine &Title = "") {
  int FD;
  // Windows can't always handle long paths, so limit the length of the name.
  std::string N = Name.str();
  N = N.substr(0, std::min<std::size_t>(N.size(), 140));
  std::string Filename = createGraphFilename(N, FD);
  raw_fd_ostream O(FD, /*shouldClose=*/ true);

  if (FD == -1) {
    errs() << "error opening file '" << Filename << "' for writing!\n";
    return "";
  }

  llvm::WriteGraph(O, G, ShortNames, Title);
  errs() << " done. \n";

  return Filename;
}

#if 0
/// ViewGraph - Emit a dot graph, run 'dot', run gv on the postscript file,
/// then cleanup.  For use from the debugger.
///
template<typename GraphType>
void ViewGraph(const GraphType &G, const Twine &Name,
               bool ShortNames = false, const Twine &Title = "",
               GraphProgram::Name Program = GraphProgram::DOT) {
  std::string Filename = llvm::WriteGraph(G, Name, ShortNames, Title);

  if (Filename.empty())
    return;

  DisplayGraph(Filename, false, Program);
}
#endif
} // end namespace llvm

#endif // LLVM_SUPPORT_GRAPHWRITER_H