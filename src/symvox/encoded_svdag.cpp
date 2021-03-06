
//=============================================================
//  This file is part of the SymVox (Symmetry Voxelization) software
//  Copyright (C) 2016 by CRS4 Visual Computing Group, Pula, Italy
//
//  For more information, visit the CRS4 Visual Computing Group 
//  web pages at http://vic.crs4.it
//
//  This file may be used under the terms of the GNU General Public
//  License as published by the Free Software Foundation and appearing
//  in the file LICENSE included in the packaging of this file.
//
//  CRS4 reserves all rights not expressly granted herein.
//  
//  This file is provided AS IS with NO WARRANTY OF ANY KIND, 
//  INCLUDING THE WARRANTY OF DESIGN, MERCHANTABILITY AND FITNESS 
//  FOR A PARTICULAR PURPOSE.
//=============================================================



#include <iterator>
#include <vector>
#include <algorithm>
#include <iostream>
#include <fstream>
#include <bitset>
#include <stack>

#include <symvox/encoded_svdag.hpp>
#include <symvox/util.hpp>

EncodedSVDAG::EncodedSVDAG() : EncodedOctree() {
	_data.clear();
}

EncodedSVDAG::~EncodedSVDAG() {
	printf("Cleaning up EncodedSVDAG...\n");
    _data.clear();
    _data.shrink_to_fit();
}

bool EncodedSVDAG::load(const std::string filename)
{
	printf("* Loading SVDAG '%s'... ", filename.c_str()); fflush(stdout);

	std::ifstream ifs;
	ifs.open(filename, std::ios::in | std::ios::binary);
	if (!ifs.is_open()) {
		printf("FAILED!!!\n");
		return false;
	}

	_data.clear();

	ifs.read((char *)_sceneBBox[0].to_pointer(), 12);
	ifs.read((char *)_sceneBBox[1].to_pointer(), 12);
	ifs.read((char *)&_rootSide, 4);
	ifs.read((char *)&_levels, 4);
	ifs.read((char *)&_nNodes, 4);
	ifs.read((char *)&_firstLeafPtr, 4);
	unsigned int count = (unsigned int)_data.size();
	ifs.read((char *)&count, 4);
	_data.resize(count);
	ifs.read((char *)_data.data(), count * sizeof(sl::uint32_t));

	printf("OK!\n");

	//std::cout << "BBOX: " << _sceneBBox[0] << " | " << _sceneBBox[1] << std::endl;
	//std::cout << "META: " << _rootSide << " , " << _levels << ", " << _nNodes << ", " << _firstLeafPtr << ", " << count << std::endl;
	//std::cout << "ROOT: " << _data[0] << " , " << _data[1] << ", " << _data[2] << ", " << _data[3] << ", " << _data[4] << ", " << _data[5] << ", " << _data[6] << ", " << _data[7] << ", " << _data[8] << std::endl;

	return true;
}

bool EncodedSVDAG::save(const std::string filename) const
{

	printf("* Saving SVDAG '%s'... ", filename.c_str()); fflush(stdout);

	std::ofstream ofs;
	ofs.open(filename, std::ios::out | std::ios::binary);
	if (!ofs.is_open()) {
		printf("FAILED!!!\n");
		return false;
	}

	ofs.write((char *)_sceneBBox[0].to_pointer(), 12);
	ofs.write((char *)_sceneBBox[1].to_pointer(), 12);
	ofs.write((char *)&_rootSide, 4);
	ofs.write((char *)&_levels, 4);
	ofs.write((char *)&_nNodes, 4);
	ofs.write((char *)&_firstLeafPtr, 4);
	unsigned int count = (unsigned int)_data.size();
	ofs.write((char *)&count, 4);
	ofs.write((char *)_data.data(), count * sizeof(sl::uint32_t));

	ofs.close();

	printf("OK!\n");

	return true;
}

void EncodedSVDAG::encode(const GeomOctree & octree) {

	printf("* Encoding to SVDAG... ");

	if (octree.getState() != GeomOctree::S_DAG) {
		printf("FAILED! Octree is not in DAG state\n");
		return;
	}

	_data.reserve(octree.getNNodes());
	_sceneBBox = octree.getSceneBBox();
	_rootSide = octree.getRootSide();
	_levels = octree.getLevels();
	_nVoxels = octree.getNVoxels();
	_nNodes = octree.getNNodes();

	const GeomOctree::NodeData &octData = octree.getNodeData();

	std::vector<sl::uint32_t> truePtrs;
	sl::uint32_t counter = 0;

	_clock.restart();

    // Asssemble list of absolute pointers in the single node list for nodes in all levels
	for (unsigned int lev = 0; lev < _levels; ++lev) {
		for (unsigned int i = 0; i < octData[lev].size(); ++i) {
			truePtrs.push_back(counter);
			//printf("%i\t%i\n", truePtrs.size() - 1, counter);
			counter += (lev < _levels - 1) ? octData[lev][i].getNChildren() + 1 : 1;
		}
	}

	_firstLeafPtr = counter;

	unsigned int levSizeAcum = 0;

    // Precompute level sizes to take into account a Node's childLevels attribute for encoding a multi-level encoded SVDAG
    std::vector<unsigned int> levSizesAcum(_levels);
    for (unsigned int lev = 0; lev < _levels; ++lev) {
        unsigned int levSize = (unsigned int)octData[lev].size();
        levSizeAcum += levSize;
        levSizesAcum[lev] = levSizeAcum;
    }

    levSizeAcum = 0;
	for (unsigned int lev = 0; lev < _levels; ++lev) {
		unsigned int levSize = (unsigned int)octData[lev].size();
		levSizeAcum += levSize;
		for (unsigned int i = 0; i < levSize; ++i) {
			const GeomOctree::Node &n = octData[lev][i];
			_data.push_back(sl::uint32_t(n.childrenBitmask));
//			printf("[%2d](%3d)[%i]", lev, dagEncoded.size() - 1, std::bitset<8>(bn.childrenBitmask).count());
			if (_data.size() < _firstLeafPtr) {
				counter = 0;
				for (int k = 7; k >= 0; --k) {
					if (n.children[k] != GeomOctree::nullNode) {
                        // Multi level merging: Add pointer for potential different the child levels
                        size_t offset = n.childLevels[k] == 0 ? 0 : levSizesAcum[n.childLevels[k] - 1];
                        _data.push_back(truePtrs[n.children[k] + offset]);

                        // Original single level merging
//                        _data.push_back(truePtrs[n.children[k] + levSizeAcum]);

                        //printf("\t%i", dagEncoded[dagEncoded.size() - 1]);
						counter++;
					}
					//else printf("\t");
				}
#if 0
				if (check) {
					if (counter != n.getNChildren() && (lev < _levels - 1)) {
						printf("\nWRONG! bitcount != not null sons!!!\n");
						printf("Lev %u  NoID: %u  bitcount: %d  Bitmaks: \n", lev, i, n.getNChildren());
						for (int k = 0; k < 8; k++) {
							printf("  * [%i] ptr: %u\n", int((n.childrenBitmask&(1 << k))), n.children[k]);

						}
						return false;
					}
				}
#endif
			}
			else {
				//printf("\t");
				//for (int k = 7; k >= 0; --k)
				//bn.childrenBitmask & (1<<k) ? printf("1") : printf("0");
			}
			//	printf("\n");
		}
	}

	_encodingTime = _clock.elapsed();

	printf("OK! [%s]\n", sl::human_readable_duration(_encodingTime).c_str());
}

GeomOctree::Node EncodedSVDAG::decodeNode(GeomOctree::id_t index, int lev) {
	GeomOctree::Node node;
	node.childrenBitmask = (sl::uint8_t) _data[index];

	if (lev != _levels - 1) {
		int childCount = 0;
		for (int i = 0; i < 8; i++) {
			if (node.existsChild(7 - i)) {
				node.children[7 - i] = _data[index + childCount + 1];
				childCount++;
			}
		}
	}
	return node;
}

GeomOctree EncodedSVDAG::decode(Scene &scene) {
	GeomOctree::NodeData data(_levels);

	// loop over all encoded nodes & keep track of which level we're at manually
	int lev = 0;

	std::vector<unsigned int> levStarts(_levels, -1); // keep track of starting positions per level, starting at MAX_UINT
	levStarts[0] = 0;


	unsigned int numPtrs = 0;
	std::vector<std::map<unsigned int, unsigned int>> numPtrsBeforeNode(_levels);

	printf("Decoding nodes...\n");
	for (unsigned int i = 0; i < _data.size(); i++) {
		if (lev < (int) _levels - 1 && i == levStarts[lev + 1]) { // if the start of the next level has been reached, increment lev
			lev++;
			numPtrs = 0;
		}
		auto n = decodeNode(i, lev);
		numPtrsBeforeNode[lev][i] = numPtrs;
		data[lev].push_back(n);

		if (lev < _levels - 1) {
			i += n.getNChildren(); // skip forward for each child pointer
			for (int c = 0; c < 8; c++) { // update the start of the next level if an earlier child pointer can be found
				if (n.existsChild(c)) {
					levStarts[lev + 1] = sl::min(levStarts[lev + 1], n.children[c]);
				}
			}
			numPtrs += n.getNChildren();
		}
	}

	// After all nodes have been decoded, recreate the true pointers
	// By removing the number of numbers before each node from each pointer to that node
	// And subtracting the acum size to make them relative to the start of the level again
	printf("Updating pointers...\n");
	unsigned int acumLevSize = 0;
	for (int lev = 0; lev < _levels - 1; ++lev) {
		acumLevSize += data[lev].size();
		for (GeomOctree::id_t nodeIndex = 0; nodeIndex < data[lev].size(); ++nodeIndex) {
			acumLevSize += data[lev][nodeIndex].getNChildren();
		}

		for (GeomOctree::id_t nodeIndex = 0; nodeIndex < data[lev].size(); ++nodeIndex) {
			auto *n = &data[lev][nodeIndex];
			for (int c = 0; c < 8; ++c) {
				if (n->existsChild(c)) {
					n->children[c] -= acumLevSize + numPtrsBeforeNode[lev + 1][n->children[c]];
				} else {
					n->children[c] = GeomOctree::nullNode;
				}
			}
		}
	}
	
	GeomOctree::Stats stats;
	stats.nTotalVoxels = _nVoxels;
	stats.nNodesDAG = _nNodes;
	stats.nNodesLastLevDAG = _nLeaves;

	GeomOctree octree(data, _sceneBBox, _rootSide, _levels, stats);
	return octree;
}



int EncodedSVDAG::getNodeIndex(sl::point3f p, int level) const
{
	TravNode curNode = getRootTravNode();
	sl::point3f nodeCenter = _sceneBBox.center();

	bool mX = false, mY = false, mZ = false;

	for (int i = 0; i < level; i++) {

		if (mX) p[0] = 2.f * nodeCenter[0] - p[0];
		if (mY) p[1] = 2.f * nodeCenter[1] - p[1];
		if (mZ) p[2] = 2.f * nodeCenter[2] - p[2];

		int selectedChild = 4 * int(p[0] > nodeCenter[0]) + 2 * int(p[1] > nodeCenter[1]) + int(p[2] > nodeCenter[2]);
		if (!hasChild(curNode, selectedChild)) {
			printf("L%u|", i);
			break;
		}
		float hs = getHalfSide(i + 1);
		nodeCenter[0] += (p[0] > nodeCenter[0]) ? hs : -1.f * hs;
		nodeCenter[1] += (p[1] > nodeCenter[1]) ? hs : -1.f * hs;
		nodeCenter[2] += (p[2] > nodeCenter[2]) ? hs : -1.f * hs;

		curNode = getChild(curNode, selectedChild, mX, mY, mZ);

		if (i == level - 2) {
			return curNode.idx;
		}
	}
	// printf("No leaf found at given position for deletion \n");
	return -1;
}

EncodedSVDAG::TravNode EncodedSVDAG::getRootTravNode() const
{
	TravNode tn;
	tn.idx = 0;
	tn.level = 0;
	return tn;
}

bool EncodedSVDAG::hasChild(const TravNode & node, const int c) const
{
	return (_data[node.idx] & (1U << c)) != 0;
}

EncodedSVDAG::TravNode EncodedSVDAG::getChild(const TravNode & node, const int c, bool & mX, bool & mY, bool & mZ) const
{
	TravNode tn;
	if (node.level >= _levels-1) {
		printf("EncodedSVDAG::getChild: WARNING! Asking for a node child, but node is in its max level\n");
		return tn;
	}
	sl::uint8_t mask = sl::uint8_t(_data[node.idx] & 0xFF);
	tn.idx = _data[node.idx + bitCount(mask >> c)];
	tn.level = node.level + 1;
	mX = mY = mZ = false;
	return tn;
}

bool EncodedSVDAG::isLeaf(const TravNode & node) const
{
	return node.level == (_levels - 1);
}

bool EncodedSVDAG::hasVoxel(const TravNode & leaf, const int i, const int j, const int k) const
{
	int c = 4 * i + 2 * j + k;
	return hasChild(leaf, c);
}

