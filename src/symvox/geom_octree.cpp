
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

#include <queue>
#include <stack>
#include <set>
#include <utility>
#include <omp.h>

#include <fstream>
#include <liblas/liblas.hpp>

#include<sl/external_array.hpp>

#include <symvox/geom_octree.hpp>
#include <symvox/test_triangle_box.hpp>
#include <symvox/util.hpp>

GeomOctree::GeomOctree(Scene * scene) : Octree(scene) {
	_state = S_EMPTY;
}


GeomOctree::GeomOctree(const GeomOctree &other) : Octree (other) {
	_state = other._state;
	_stats = other._stats;
	_data.reserve(other._data.size());
	_data = other._data;
}

void GeomOctree::buildSVOFromPoints(std::string fileName, unsigned int levels, sl::aabox3d bbox, bool internalCall, std::vector< sl::point3d > * leavesCenters) {
    if (!internalCall) printf("* Building SVO... "); fflush(stdout);

    _bbox = sl::conv_to<sl::aabox3f>::from(bbox);
    _levels = levels;
    sl::vector3f sides =_bbox.half_side_lengths() * 2.0f;
    _rootSide = sl::max(sl::max(sides[0], sides[1]), sides[2]);

    _data.resize(_levels);

    Node root;
    _data[0].push_back(root);

    struct QueueItem {
        QueueItem(id_t id, sl::uint8_t l, sl::point3d c) : nodeID(id), level(l), center(c) {}
        id_t nodeID;
        sl::uint8_t level;
        sl::point3d center;
    };
    sl::point3d childrenCenters[8];
    std::stack<QueueItem> queue;

    if(!internalCall) _clock.restart();

    // Prepare for reading the file
    std::ifstream ifs;
    ifs.open(fileName, std::ios::in | std::ios::binary);

    liblas::ReaderFactory f;
    liblas::Reader reader = f.CreateWithStream(ifs);

    int numPoints = reader.GetHeader().GetPointRecordsCount();
    int stepLogger = (int)round(numPoints / 10.f);

    // For every point
    unsigned int iPoint = 0;
    while (reader.ReadNextPoint()) {
        liblas::Point const& p = reader.GetPoint();

        if (!internalCall && (iPoint % stepLogger == 0)) {
            printf("%.0f%%..", round(100.f * (iPoint / (float)numPoints)));
            fflush(stdout);
        }

        // Push the root node to a queue
        queue.push(QueueItem(0, 0, bbox.center()));

        // Traverse through all nodes in the tree that intersect with the triangle, starting at the root
        while (!queue.empty()) {
            const QueueItem qi = queue.top(); queue.pop();

            // Store of the position (center) of all child nodes of the current node
            double k = getHalfSideD(qi.level + 1);
            childrenCenters[PXPYPZ] = qi.center + sl::vector3d(+k, +k, +k);
            childrenCenters[PXPYNZ] = qi.center + sl::vector3d(+k, +k, -k);
            childrenCenters[PXNYPZ] = qi.center + sl::vector3d(+k, -k, +k);
            childrenCenters[PXNYNZ] = qi.center + sl::vector3d(+k, -k, -k);
            childrenCenters[NXPYPZ] = qi.center + sl::vector3d(-k, +k, +k);
            childrenCenters[NXPYNZ] = qi.center + sl::vector3d(-k, +k, -k);
            childrenCenters[NXNYPZ] = qi.center + sl::vector3d(-k, -k, +k);
            childrenCenters[NXNYNZ] = qi.center + sl::vector3d(-k, -k, -k);

            // Traverse through all child nodes
            Node & node = _data[qi.level][qi.nodeID];
            for (int i = 7; i >= 0; --i) {
                // If the point lies inside this child...
                if ((p.GetX() >= childrenCenters[i][0] -k && p.GetX() < childrenCenters[i][0] +k)
                 && (p.GetY() >= childrenCenters[i][1] -k && p.GetY() < childrenCenters[i][1] +k)
                 && (p.GetZ() >= childrenCenters[i][2] -k && p.GetZ() < childrenCenters[i][2] +k)) {
                    // Mark the child mask
                    node.setChildBit(i);
                    // If there is no child node inserted yet, and it's not a leaf, insert a child node
                    if (!node.existsChildPointer(i) && (qi.level < (_levels - 1))) {
                        node.children[i] = (id_t)_data[qi.level + 1].size();
                        _data[qi.level + 1].emplace_back();
                        _nNodes++;
                        if (leavesCenters!=NULL && (qi.level == (_levels - 2))) leavesCenters->push_back(childrenCenters[i]);
                    }
                    // If this child is not a leaf, continue intersecting its children in a future iteration
                    if((qi.level+1U) < _levels) queue.push(QueueItem(node.children[i], qi.level + 1, childrenCenters[i]));
                }
            }
        }
        iPoint++;
    }

    cleanEmptyNodes();
    if (!internalCall) _stats.buildSVOTime = _clock.elapsed();

    // compute NVoxels
    for (id_t i = 0; i < _data[_levels - 1].size(); ++i) {
        _nVoxels += _data[_levels - 1][i].getNChildren();
    }

    _state = S_SVO;
    _stats.nNodesSVO = _nNodes;
    _stats.nNodesLastLevSVO = _data[_levels - 1].size();
    _stats.simulatedEncodedSVOSize = (_stats.nNodesSVO - _stats.nNodesLastLevSVO) * 4;
    _stats.nTotalVoxels = _nVoxels;

    if(!internalCall) printf("OK! [%s]\n",sl::human_readable_duration(_stats.buildSVOTime).c_str());
}

void GeomOctree::buildSVO(unsigned int levels, sl::aabox3d bbox, bool internalCall, std::vector< sl::point3d > * leavesCenters, bool putMaterialIdInLeaves) {
	
	if (!internalCall) printf("* Building SVO... "); fflush(stdout);

	_bbox = sl::conv_to<sl::aabox3f>::from(bbox);
	_levels = levels;
	sl::vector3f sides =_bbox.half_side_lengths() * 2.0f;
	_rootSide = sl::max(sl::max(sides[0], sides[1]), sides[2]);
	
	_data.resize(_levels);

	Node root;
	_data[0].push_back(root);

	struct QueueItem {
		QueueItem(id_t id, sl::uint8_t l, sl::point3d c) : nodeID(id), level(l), center(c) {}
		id_t nodeID;
		sl::uint8_t level;
		sl::point3d center;
	};
	sl::point3d childrenCenters[8];
	std::stack<QueueItem> queue;

	if(!internalCall) _clock.restart();

	int stepLogger = (int)round(_scene->getNRawTriangles() / 10.f);
    // For every triangle...
    for (std::size_t iTri = 0; iTri < _scene->getNRawTriangles(); iTri++) {
		if (!internalCall && (iTri % stepLogger == 0)) {
			printf("%.0f%%..", round(100.f * (iTri / (float)_scene->getNRawTriangles())));
			fflush(stdout);
		}
		unsigned int triMatId;
		if (putMaterialIdInLeaves) triMatId = (unsigned int)_scene->getTriangleMaterialId(iTri);

        // Push the root node to a queue
		queue.push(QueueItem(0, 0, bbox.center()));

        // Traverse through all nodes in the tree that intersect with the triangle, starting at the root
		while (!queue.empty()) {
			const QueueItem qi = queue.top(); queue.pop();

            // Store of the position (center) of all child nodes of the current node
			double k = getHalfSideD(qi.level + 1);
			childrenCenters[PXPYPZ] = qi.center + sl::vector3d(+k, +k, +k);
			childrenCenters[PXPYNZ] = qi.center + sl::vector3d(+k, +k, -k);
			childrenCenters[PXNYPZ] = qi.center + sl::vector3d(+k, -k, +k);
			childrenCenters[PXNYNZ] = qi.center + sl::vector3d(+k, -k, -k);
			childrenCenters[NXPYPZ] = qi.center + sl::vector3d(-k, +k, +k);
			childrenCenters[NXPYNZ] = qi.center + sl::vector3d(-k, +k, -k);
			childrenCenters[NXNYPZ] = qi.center + sl::vector3d(-k, -k, +k);
			childrenCenters[NXNYNZ] = qi.center + sl::vector3d(-k, -k, -k);

            // Traverse through all child nodes
			Node & node = _data[qi.level][qi.nodeID];
			for (int i = 7; i >= 0; --i) {
                // If the triangle intersects with this child...
				if (_scene->getTrianglePtr(iTri) != NULL && testTriBox(childrenCenters[i], k, _scene->getTrianglePtr(iTri))) {
                    // Mark the child mask
                    node.setChildBit(i);
                    // If there is no child node inserted yet, and it's not a leaf, insert a child node
					if (!node.existsChildPointer(i) && (qi.level < (_levels - 1))) {
						node.children[i] = (id_t)_data[qi.level + 1].size();
						_data[qi.level + 1].emplace_back();
						_nNodes++;
						if (leavesCenters!=NULL && (qi.level == (_levels - 2))) leavesCenters->push_back(childrenCenters[i]);
					}
                    // If this child is not a leaf, continue intersecting its children in a future iteration
					if((qi.level+1U) < _levels) queue.push(QueueItem(node.children[i], qi.level + 1, childrenCenters[i]));
					else {
						if (putMaterialIdInLeaves) node.children[i] = (id_t)triMatId;
#if 0 // outputs a debug obj of voxels as points with their colours
						sl::color3f c;
						_scene->getTriangleColor(iTri, c);
						printf("v %f %f %f %f %f %f\n", childrenCenters[i][0], childrenCenters[i][1], childrenCenters[i][2], c[0], c[1], c[2]);
#endif
					}
				}
			}
		}
	}

	cleanEmptyNodes();
	if (!internalCall) _stats.buildSVOTime = _clock.elapsed();

	// compute NVoxels
	for (id_t i = 0; i < _data[_levels - 1].size(); ++i) {
		_nVoxels += _data[_levels - 1][i].getNChildren();
	}
	
	_state = S_SVO;
	_stats.nNodesSVO = _nNodes;
	_stats.nNodesLastLevSVO = _data[_levels - 1].size();
	_stats.simulatedEncodedSVOSize = (_stats.nNodesSVO - _stats.nNodesLastLevSVO) * 4;
	_stats.nTotalVoxels = _nVoxels;

	if(!internalCall) printf("OK! [%s]\n",sl::human_readable_duration(_stats.buildSVOTime).c_str());
}

/**
 * @brief GeomOctree::buildDAG The main function that builds sub-SVOs, individually reduces them to DAGs and merges them
 * @param levels
 * @param stepLevel
 * @param bbox
 * @param verbose
 */
void GeomOctree::buildDAG(unsigned int levels, unsigned int stepLevel, sl::aabox3d bbox, bool verbose) {
	
	printf("* Building DAG [stepLevel: %i]\n", stepLevel); fflush(stdout);
	
	std::vector< sl::point3d > leavesCenters;
	
	unsigned int stepLevels = stepLevel + 1;

	_clock.restart();
	sl::time_duration timeStamp = _clock.elapsed();

	printf("\t- Building root SVO subtree... ");
	
	buildSVO(stepLevels, bbox, true, &leavesCenters);
	
	printf("OK! [%s]\n", sl::human_readable_duration(_clock.elapsed() - timeStamp).c_str()); fflush(stdout);
	

	double lhs = getHalfSideD(stepLevels - 1);
	sl::vector3d corners[8];
	corners[PXPYPZ] = sl::vector3d(+lhs, +lhs, +lhs); // PXPYPZ
	corners[PXPYNZ] = sl::vector3d(+lhs, +lhs, -lhs); // PXPYNZ
	corners[PXNYPZ] = sl::vector3d(+lhs, -lhs, +lhs); // PXNYPZ
	corners[PXNYNZ] = sl::vector3d(+lhs, -lhs, -lhs); // PXNYNZ
	corners[NXPYPZ] = sl::vector3d(-lhs, +lhs, +lhs); // NXPYPZ
	corners[NXPYNZ] = sl::vector3d(-lhs, +lhs, -lhs); // NXPYNZ
	corners[NXNYPZ] = sl::vector3d(-lhs, -lhs, +lhs); // NXNYPZ
	corners[NXNYNZ] = sl::vector3d(-lhs, -lhs, -lhs); // NXNYNZ

	_stats.nNodesSVO = _nNodes;
	_stats.nNodesLastLevSVO = 0;
	std::map<unsigned int, GeomOctree *> leavesOctrees;

	float nodesProcessed = 0;
	const size_t nodesToProcess = _nVoxels;
	size_t memConsumed = 0;
	size_t acumSubtreesDAGNNodes = 0;

	printf("\t- Building %zu subtress (SVO->DAG) [%i threads]... ", _nVoxels, (int)omp_get_max_threads());
	if (verbose) printf("\n");
	timeStamp = _clock.elapsed();
	float acumDAGTimeFactor = 0;
#if 1 // More memory consumtion, but ensures CPUs occupancy
#pragma omp parallel for schedule(dynamic,2)
#else
#pragma omp parallel for schedule(static)
#endif
	for (int i = 0; i < _data[_levels - 1].size(); ++i) {
		for (int j = 7; j >= 0; --j) {
			if (_data[_levels - 1][i].existsChild(j)) {
				sl::time_point threatInitTime = _clock.now();
				GeomOctree * leafOctree = new GeomOctree(_scene);
				sl::aabox3d lbbox;
				lbbox.to_empty();
				lbbox.merge(leavesCenters[i]);
				lbbox.merge(leavesCenters[i] + corners[j]);
				leafOctree->buildSVO(levels - stepLevels, lbbox, true);
				size_t nNodesLeafSVO = leafOctree->getNNodes();
				size_t nNodesLeafLastLevSVO = leafOctree->_data[leafOctree->_data.size() - 1].size();
#pragma omp atomic
				_stats.nNodesSVO += nNodesLeafSVO;
#pragma omp atomic
				_stats.nNodesLastLevSVO += nNodesLeafLastLevSVO;
				size_t incVoxels = leafOctree->getNVoxels() - 1;
#pragma omp atomic
				_nVoxels += incVoxels; // root doesn't count

				leafOctree->toDAG(true);

#pragma omp atomic		
				nodesProcessed += 1.f;
				size_t incNNodes = leafOctree->getNNodes();
#pragma omp atomic		
				acumSubtreesDAGNNodes += incNNodes;
				
#pragma omp atomic
				acumDAGTimeFactor += leafOctree->getStats().toDAGTime.as_microseconds() / (float) (_clock.now()- threatInitTime).as_microseconds();
#pragma omp critical
				{
					leavesOctrees[i * 8 + j] = leafOctree;
					memConsumed += leafOctree->getMemFootprint();
					_stats.memFootprint = sl::max(_stats.memFootprint, leafOctree->getMemFootprint());
					if (verbose) {
						printf("\t\t- [%3d%%] [%3u-%u / %zu (%d)]\t%zu -> %zu\n", int(100.f * (nodesProcessed / (float)nodesToProcess)), i, j, _data[_levels - 1].size(), omp_get_thread_num(), nNodesLeafSVO, leafOctree->getNNodes());
						fflush(stdout);
					}
				}
			}
		}
	}
	acumDAGTimeFactor /= nodesProcessed;
	sl::time_duration acumDAGTime = _clock.elapsed() * acumDAGTimeFactor;

	if(verbose)
		printf("\t\t- [100%%] All subtrees done! [%s]\n", sl::human_readable_duration(_clock.elapsed() - timeStamp).c_str());
	else
		printf("OK! [%s]\n", sl::human_readable_duration(_clock.elapsed() - timeStamp).c_str());

	_stats.simulatedEncodedSVOSize = (_stats.nNodesSVO - _stats.nNodesLastLevSVO) * 4;
	_stats.nTotalVoxels = _nVoxels;

	_data.resize(levels);
	_levels = levels;

	timeStamp = _clock.elapsed();

	printf("\t- Joining subtrees... ");

	for (int i = 0; i < _data[stepLevels - 1].size(); ++i) {
		for (int j = 7; j >= 0; --j) {
			if (_data[stepLevels - 1][i].existsChild(j)) {
				GeomOctree * oct = leavesOctrees[i * 8 + j];
				_data[stepLevels - 1][i].children[j] = (id_t)_data[stepLevels].size();
				for (unsigned int k = stepLevels; k < levels; ++k) {
					if (k < (levels - 1)) { // update pointers
						id_t offset = (id_t)_data[k + 1].size();
						for (unsigned int m = 0; m < oct->_data[k - stepLevels].size(); ++m) {
							for (unsigned int n = 0; n < 8; ++n) {
								if (oct->_data[k - stepLevels][m].existsChildPointer(n))
									oct->_data[k - stepLevels][m].children[n] += offset;
							}
						}
					}
					_data[k].insert(_data[k].end(), oct->_data[k - stepLevels].begin(), oct->_data[k - stepLevels].end());
				}
				delete oct;
			}
		}
	}

	printf("OK! [%s]\n", sl::human_readable_duration(_clock.elapsed() - timeStamp).c_str());
	
	printf("\t- Last DAG pass to whole subtree... ");
	acumDAGTime += (_clock.elapsed() - timeStamp);
	timeStamp = _clock.elapsed();
	
	toDAG(true);

	acumDAGTime += (_clock.elapsed() - timeStamp);
	_stats.toDAGTime = acumDAGTime;
	printf("OK! [%s]\t(%zu -> %zu)\n", sl::human_readable_duration(_clock.elapsed() - timeStamp).c_str(), acumSubtreesDAGNNodes, _nNodes);

	_stats.buildDAGTime = _clock.elapsed();

	printf("\t- Finished! Total time [%s]\n", sl::human_readable_duration(_stats.buildDAGTime).c_str());

}

unsigned int GeomOctree::cleanEmptyNodes() {
	std::set<id_t> emptyNodes;
	unsigned int nDelPtrs = 0;
	for (int lev = _levels - 1; lev > 0; --lev) {
		emptyNodes.clear();
		for (id_t i = 0; i < _data[lev].size(); ++i) {
			if (!_data[lev][i].hasChildren()) emptyNodes.insert(i);
		}
		for (id_t i = 0; i < _data[lev-1].size(); ++i) {
			for (int j = 0; j < 8; ++j) {
				if (emptyNodes.find(_data[lev - 1][i].children[j]) != emptyNodes.end()) {
					_data[lev-1][i].unsetChildBit(j);
					_data[lev-1][i].children[j] = nullNode;
					nDelPtrs++;
				}
			}
		}
	}
	return nDelPtrs;
}


/**
 * @brief GeomOctree::toDAG Reduces a SVO to a lossy DAG
 * @param iternalCall Whether the function is called internally (only adds extra print)
 */
void GeomOctree::toLossyDAG(bool iternalCall) {

    if (_state == S_SVO) {
        if (!iternalCall) printf("* Transforming SVO -> Lossy DAG ... "); fflush(stdout);
    } else {
        printf("ERROR! This is not a SVO!\n");
        return;
    }

    /**
     * Main idea:
     * - Merge nodes that are similar (e.g. only have 1 child difference)
     * - Most effective to perform on most common nodes
     *
     * Implementation ideas
     * - Represent every child bit as a float, how close it is to the true geometry
     * -
     *
     * Proof of concept, greedy: Merge the largest sets of nodes that have diff 1
     * - Just per level for now, not earlier levels
     *
     * Looking at #nodes compressed per level w/ lossless compression, lossy compression should be avoided on:
     * - the leaves, there are very little benefits with large visual side effects
     * - the first half of levels, since this will have the largest side effects
     * -> therefore, focus on the deepest levels above the leaves
     */

    _nNodes = 1;
    /** Vector of duplicate nodes. Every index denotes the first duplicate of the node at that index in per level. */
    std::vector<id_t> correspondences;
    std::map<Node, id_t> uniqueNodesChecker;
    std::vector<Node> uniqueNodes;
    std::vector<sl::uint32_t> uniqueRefCount; // count for each uniqueNode how many refs it has

    std::vector<Node> lossyUniqueNodes;
    std::map<Node, id_t> uniqueCorrIndices;

    sl::time_point ts = _clock.now();

    printf("\n");

    // For every level, starting at the leaves...
    for (unsigned int lev = _levels - 1; lev > 0; --lev) {
        // Clear the lists used to keep track of correspondeces etc
        size_t oldLevSize = _data[lev].size();
        uniqueNodes.clear();
        uniqueNodes.shrink_to_fit();
        uniqueNodesChecker.clear();
        correspondences.clear();
        correspondences.resize(oldLevSize);

        uniqueRefCount.clear();
        uniqueRefCount.shrink_to_fit();
        lossyUniqueNodes.clear();
        lossyUniqueNodes.shrink_to_fit();
        uniqueCorrIndices.clear();

        // For all nodes in this level...
        for (id_t i = 0; i < _data[lev].size(); i++) {
            Node n = _data[lev][i];
            if (!n.hasChildren()) continue; // skip empty nodes
            auto k = uniqueNodesChecker.find(n); // find if duplicate

            if (k != uniqueNodesChecker.end()) { // found
                correspondences[i] = (*k).second; // store duplicate node
                uniqueRefCount[(*k).second] += 1;
            }
            else { // !found
                uniqueCorrIndices[n] = uniqueNodes.size();
                uniqueNodesChecker[n] = (id_t)uniqueNodes.size(); // store it as unique node
                correspondences[i] = (id_t)uniqueNodes.size(); // the correspondence is this node itself
                uniqueNodes.push_back(n);
                uniqueRefCount.push_back(1);
            }
        }

        uniqueNodes.shrink_to_fit();
        printf("Reduced level %u \tfrom %lu  \tto %lu nodes ", lev, _data[lev].size(), uniqueNodes.size());

        // Clear original SVO nodes
        _data[lev].clear();
        _data[lev].shrink_to_fit();

        // ===== LOSSY COMPRESSION =====
        bool doLossy = lev == (_levels -1); // && lev > _levels / 2; // only lossy on deepest 1/2 levels, except leaves

        if (doLossy) {
            // === Sort unique nodes on #references ===
            // https://stackoverflow.com/questions/26569801/sort-one-array-based-on-values-of-another-array
            std::vector<std::pair<Node, sl::uint32_t>> order(uniqueNodes.size());
            for (int i=0; i<uniqueNodes.size(); ++i){
                order[i] = std::make_pair(uniqueNodes[i], uniqueRefCount[i]);
            }
            struct ordering {
                bool operator ()(std::pair<Node, sl::uint32_t> const& a,
                                 std::pair<Node, sl::uint32_t> const& b) {
                    return a.second < b.second;
                }
            };
            std::sort(order.begin(), order.end(), ordering());

            // === Check diff between each, merge those with diff 1 ===
            // Keep track of nodes that are merged
            uniqueNodesChecker.clear();
            correspondences.clear();
            correspondences.resize(uniqueNodes.size());

            // For every unique node, starting with the most referenced one...
            for (int i = 0; i < order.size(); ++i) {

                // If node i already merged, skip
                if (uniqueNodesChecker.find(order[i].first) != uniqueNodesChecker.end()) {
                    continue;
                }

                // Compare it with every other node...
                for (int j = i + 1; j < order.size(); ++j) {
                    // If node j already merged, skip
                    if (uniqueNodesChecker.find(order[j].first) != uniqueNodesChecker.end()) {
                        continue;
                    }

                    // Compute the diff...
                    int diff = 0;
                    for (int c = 0; c < 8; ++c) {
                        if (order[i].first.children[c] != order[j].first.children[c]) {
                            diff++;
                        }
                    }
                    // If diff is 1, merge!
                    if (diff <= 1) {
                        auto corIdx = uniqueCorrIndices.find(order[j].first)->second;
                        correspondences[corIdx] = (id_t)lossyUniqueNodes.size();

                        uniqueNodesChecker[order[j].first] = 0; // mark it as merged

    //                    printf("Merged!\n");
                    }
                }
                // Add as lossy unique node
                uniqueNodesChecker[order[i].first] = 0; // mark it as merged
                auto n = order[i].first;
                auto corIdx = uniqueCorrIndices.find(order[i].first)->second;
                correspondences[corIdx] = (id_t)lossyUniqueNodes.size(); // the correspondence is this node itself
                lossyUniqueNodes.push_back(n);
            }

            lossyUniqueNodes.shrink_to_fit();
            printf("\t-> Lossy %lu \t(%.2f\%)\n", lossyUniqueNodes.size(), (lossyUniqueNodes.size() / float(uniqueNodes.size())) * 100.0);
            _data[lev] = lossyUniqueNodes; // Replace all SVO nodes with the unique DAG nodes in this level
        } else {
            _data[lev] = uniqueNodes; // Replace all SVO nodes with the unique DAG nodes in this level
            printf("\t-> No lossy compression\n");
        }

        _data[lev].shrink_to_fit();
        _nNodes += _data[lev].size();

        // Update all pointers in the level above
        for (id_t i = 0; i < _data[lev-1].size(); i++) {
            Node * bn = &_data[lev-1][i];
            // For all children...
            for (int j = 0; j < 8; j++) {
                // If this child exists...
                if (bn->existsChild(j)) {
                    // Set the child pointer to the unique node that replaced this child
                    bn->children[j] = correspondences[bn->children[j]];
                }
            }
        }
    }

    _stats.toDAGTime = _clock.now() - ts;

    _state = S_DAG;
    _stats.nNodesDAG = _nNodes;

    if (!iternalCall) printf("OK! [%s]\n", sl::human_readable_duration(_stats.toDAGTime).c_str());
}

/** Recursive subtree comparison **/
bool GeomOctree::compareSubtrees(unsigned int levA, unsigned int levB, Node &nA, Node &nB) {
//    printf("%u, %u\n", levA, levB);

    // If B terminates before A, they are not equal since one or more levels are lost
    if (nA.hasChildren() && !nB.hasChildren()) {
        return false;
    } else if (!nA.hasChildren() || !nB.hasChildren()) {
        // If they both don't have children, simply compare their child masks
        return nA.childrenBitmask == nB.childrenBitmask;
    }

    unsigned int childLevA = levA + 1;
    unsigned int childLevB = levB + 1;

    // If the end of the graph is reached, they are equal (?)
    if (childLevB >= _levels || childLevA >= _levels) {
        return true;
    }

    for (int i = 0; i < 8; ++i) {
        // If they child bits don't match, they are not equal
        if (nA.existsChild(i) != nB.existsChild(i)) {
            return false;
        }
        // Otherwise they are equal.
        // If either one is not set, we cannot compare them further. Therefore they can be seen as equal
        else if (!(nA.existsChild(i) || nB.existsChild(i))) {
            continue;
        }

        Node &cA = _data[childLevA][nA.children[i]];
        Node &cB = _data[childLevB][nB.children[i]];

        // Compare their child masks...
        if (cA.childrenBitmask != cB.childrenBitmask) {
            return false;
        } else {
            // If both nodes have children...
            if (cA.hasChildren() && cB.hasChildren()) {

                // Then compare the subtrees of these children
                if (!this->compareSubtrees(childLevA, childLevB, cA, cB)) {
                    return false;
                }
            }
            // If only 1 node has children, they can be seen as equal. One has more detail than the other
        }
    }
    return true;
}

/**
 * @brief findAllDuplicateSubtrees Brute force search over all levels, looking for equal subtrees
 * Goal: Research to see what the benefit of multi-level merging would be.
 * Just for SVDAGs now, but SSVDAGs would likely perform better - harder to check though
 */
unsigned int GeomOctree::findAllDuplicateSubtrees() {

    // Note: Comparing ALL combinations of subtree pairs is not what we want
    // If a subtree high-up is equal to another subtree, all of its subtrees will equal it as well

    // Solution(?): Keep track of children that are NOT equal to another subtree
    // Only compare those instead of all nodes in the next level
    std::vector<Node> currentNodesToCheck;
    std::vector<Node> nextNodesToCheck;

    // Initialize all nodes of the highest level for comparison to subtrees at other levels
    unsigned int levA = 1;
    for (id_t i = 0; i < _data[levA].size(); i++) {
        Node &n = _data[levA][i];
        currentNodesToCheck.push_back(n);
    }

    unsigned int numEqualSubtrees = 0;
    unsigned int numTotalComparisons = 0;

    // Most efficient methinks:
    // Start at level 1 (A), compare to all nodes at levels above it (B), repeat for following levels
    // This way, the matches are found as early as possible

    // For every node to check...
        // Check for any level if there is an equal subtree
        // Note: If node B terminates before node A, they are NOT equal. The other way around is fine
        // For nodes that are not equal, add their children in the next level to the nextNodesToCheck list!

    std::vector<int> nodesEqualPerLevel(_levels, 0);

    for (; levA < _levels; ++levA) {
        printf("Comparing level %u\n", levA);
        // For all nodes to be checked...
        for (auto& nA : currentNodesToCheck) {
            bool foundMatch = false;
            for (unsigned int levB = 0; levB < levA; ++levB) {
                // For all nodes in the other level above A...
                for (id_t j = 0; j < _data[levB].size(); j++) {
                    numTotalComparisons++;

                    Node &nB = _data[levB][j];

                    // Brute force check equality of subtrees of nA and nB
                    bool areSubtreesEqual = compareSubtrees(levA, levB, nA, nB);

                    if (areSubtreesEqual) {
                        foundMatch = true;
                        numEqualSubtrees++;
                        nodesEqualPerLevel[levA]++;
                        break;
                    }
                }
                if (foundMatch) {
                    // If a match is found, we are done for this subtree
                    // It is equal to a subtree higher up in the graph!
                    break;
                }
            }
            if (!foundMatch) {
                // If no match was found, try later for all child nodes
                for (int i = 0; i < 8; ++i) {
                    if (nA.existsChild(i)) {
                        nextNodesToCheck.push_back(_data[levA + 1][nA.children[i]]);
                    }
                }
            }
        }

        // Since children of a node may also be children of other nodes in a DAG,
        // we need to ensure children are only present once to the nextNodesToCheck vector
        std::sort(nextNodesToCheck.begin(), nextNodesToCheck.end() );
        nextNodesToCheck.erase(std::unique(nextNodesToCheck.begin(), nextNodesToCheck.end() ), nextNodesToCheck.end());

        // After all nodes for this level have been checked, swap the current and next nodes to check
        currentNodesToCheck.clear();
        currentNodesToCheck.swap(nextNodesToCheck);
    }

    printf("Nodes equal to another one: %u. Total #nodes %zu. Total #comparisons: %u\n", numEqualSubtrees, _nNodes, numTotalComparisons);

    for (unsigned int i = 0; i < _levels; ++i) {
        double pct = 100 * (nodesEqualPerLevel[i] / double(_data[i].size()));
        printf("- Level %u:   \t%i / %zu (%.2f%%) subtrees are equal to a subtree higher up\n", i, nodesEqualPerLevel[i], _data[i].size(), pct);
    }
    printf("Total equal: %u / %zu (%.2f%%)\n", numEqualSubtrees, _nNodes, (100 * numEqualSubtrees / double(_nNodes)));

    return numEqualSubtrees;
}


/**
 * @brief GeomOctree::toDAG Reduces an SVO to a DAG
 * @param iternalCall Whether the function is called internally (only adds extra print)
 */
void GeomOctree::toDAG(bool iternalCall) {

	if (_state == S_SVO) {
		if (!iternalCall) printf("* Transforming SVO -> DAG ... "); fflush(stdout);
	} else {
		printf("ERROR! This is not a SVO!\n");
		return;
	}

	_nNodes = 1;
    /** Vector of duplicate nodes. Every index denotes the first duplicate of the node at that index in per level. */
	std::vector<id_t> correspondences;
	std::map<Node, id_t> uniqueNodesChecker;
	std::vector<Node> uniqueNodes;

	 sl::time_point ts = _clock.now();

     printf("\n");

    // For every level, starting at the leaves...
	for (unsigned int lev = _levels - 1; lev > 0; --lev) {
        // Clear the lists used to keep track of correspondeces etc
		size_t oldLevSize = _data[lev].size();
		uniqueNodes.clear();
		uniqueNodes.shrink_to_fit();
		uniqueNodesChecker.clear();
		correspondences.clear();
		correspondences.resize(oldLevSize);

        // For all nodes in this level...
		for (id_t i = 0; i < _data[lev].size(); i++) {
			Node n = _data[lev][i];
			if (!n.hasChildren()) continue; // skip empty nodes
            auto k = uniqueNodesChecker.find(n); // find if duplicate

			if (k != uniqueNodesChecker.end()) { // found
                correspondences[i] = (*k).second; // store duplicate node
			}
			else { // !found
                uniqueNodesChecker[n] = (id_t)uniqueNodes.size(); // store it as unique node
                correspondences[i] = (id_t)uniqueNodes.size(); // the correspondence is this node itself
				uniqueNodes.push_back(n);
			}
		}

        printf("Reduced level %u from %lu to %lu nodes\n", lev, _data[lev].size(), uniqueNodes.size());

		_data[lev].clear();
		_data[lev].shrink_to_fit();
		uniqueNodes.shrink_to_fit();
        _data[lev] = uniqueNodes; // Replace all SVO nodes with the unique DAG nodes in this level
		_data[lev].shrink_to_fit();
		_nNodes += _data[lev].size();
		
        // Update all pointers in the level above
		for (id_t i = 0; i < _data[lev-1].size(); i++) {
			Node * bn = &_data[lev-1][i];
            // For all children...
			for (int j = 0; j < 8; j++) {
                // If this child exists...
                if (bn->existsChild(j)) {
                    // Set the child pointer to the unique node that replaced this child
                    bn->children[j] = correspondences[bn->children[j]];
                }
			}
		}
	}

	_stats.toDAGTime = _clock.now() - ts;
	
	_state = S_DAG;
	_stats.nNodesDAG = _nNodes;

	if (!iternalCall) printf("OK! [%s]\n", sl::human_readable_duration(_stats.toDAGTime).c_str());
}


void GeomOctree::toSDAG(bool internalCall) {

	if (!internalCall) {
		if (_state == S_SVO) {
			printf("* Transforming SVO -> SDAG (not best way)... "); fflush(stdout);
		}
		else if (_state == S_DAG) {
			printf("* Transforming DAG -> SDAG (normal)... "); fflush(stdout);
		}
		else {
			printf("ERROR! This is not a DAG or SDAG!\n");
			return;
		}
	}

	struct MirroredNode{
		MirroredNode() : mirrorX(false), mirrorY(false), mirrorZ(false), id(0) {}
		MirroredNode(bool mX, bool mY, bool mZ, id_t id) :
			mirrorX(mX), mirrorY(mY), mirrorZ(mZ), id(id) {}
		bool mirrorX, mirrorY, mirrorZ;
		id_t id;
	};

	std::vector<MirroredNode> correspondences;
	std::map<Node, id_t> uniqueNodesChecker;
	std::vector<Node> uniqueNodes;

	_nNodes = 0;

	if (!internalCall) _clock.restart();

	for (unsigned int lev = _levels - 1; lev > 0; lev--) {
		unsigned int oldLevSize = (unsigned int)_data[lev].size();
		uniqueNodes.clear();
		uniqueNodesChecker.clear();
		correspondences.resize(oldLevSize);

		for (id_t i = 0; i < _data[lev].size(); i++) {

			Node n = _data[lev][i];
			if (!n.hasChildren()) continue; // skip empty nodes

			// Check invariances
			if (n == n.mirror(true, false, false)) n.setInvariantBit(0);
			if (n == n.mirror(false, true, false)) n.setInvariantBit(1);
			if (n == n.mirror(false, false, true)) n.setInvariantBit(2);

			Node niX = n.mirror(true, false, false);
			Node niY = n.mirror(false, true, false);
			Node niZ = n.mirror(false, false, true);
			Node niXY = niX.mirror(false, true, false);
			Node niXZ = niX.mirror(false, false, true);
			Node niYZ = niY.mirror(false, false, true);
			Node niXYZ = niXY.mirror(false, false, true);

			// Check children invariances
			if (lev < (_levels - 1)) {
				invertInvs(niX, lev, true, false, false);
				invertInvs(niY, lev, false, true, false);
				invertInvs(niZ, lev, false, false, true);
				invertInvs(niXY, lev, true, true, false);
				invertInvs(niXZ, lev, true, false, true);
				invertInvs(niYZ, lev, false, true, true);
				invertInvs(niXYZ, lev, true, true, true);
			}

			std::map<GeomOctree::Node, sl::uint32_t>::iterator it;
			if ((it = uniqueNodesChecker.find(n)) != uniqueNodesChecker.end()) {
				correspondences[i] = MirroredNode(false, false, false, (*it).second);
			}
			else if ((it = uniqueNodesChecker.find(niX)) != uniqueNodesChecker.end()) {
				correspondences[i] = MirroredNode(true, false, false, (*it).second);
			}
			else if ((it = uniqueNodesChecker.find(niY)) != uniqueNodesChecker.end()) {
				correspondences[i] = MirroredNode(false, true, false, (*it).second);
			}
			else if ((it = uniqueNodesChecker.find(niZ)) != uniqueNodesChecker.end()) {
				correspondences[i] = MirroredNode(false, false, true, (*it).second);
			}
			else if ((it = uniqueNodesChecker.find(niXY)) != uniqueNodesChecker.end()) {
				correspondences[i] = MirroredNode(true, true, false, (*it).second);
			}
			else if ((it = uniqueNodesChecker.find(niXZ)) != uniqueNodesChecker.end()) {
				correspondences[i] = MirroredNode(true, false, true, (*it).second);
			}
			else if ((it = uniqueNodesChecker.find(niYZ)) != uniqueNodesChecker.end()) {
				correspondences[i] = MirroredNode(false, true, true, (*it).second);
			}
			else if ((it = uniqueNodesChecker.find(niXYZ)) != uniqueNodesChecker.end()) {
				correspondences[i] = MirroredNode(true, true, true, (*it).second);
			}
			else { // !found
				uniqueNodesChecker[n] = (id_t)uniqueNodes.size();
				correspondences[i] = MirroredNode(false, false, false, id_t(uniqueNodes.size()));
				uniqueNodes.push_back(n);
			}
		}
		_data[lev].clear();
		_data[lev] = uniqueNodes;
		_data[lev].shrink_to_fit();
		_nNodes += _data[lev].size();

		for (id_t i = 0; i < _data[lev - 1].size(); i++) {
			Node &n = _data[lev - 1][i];
			for (id_t j = 0; j < 8; j++) {
				if (n.existsChildPointer(j)) {
					MirroredNode mn = correspondences[n.children[j]];
					n.children[j] = mn.id;
					if (mn.mirrorX) n.setChildMirrrorBit(0, j);
					if (mn.mirrorY) n.setChildMirrrorBit(1, j);
					if (mn.mirrorZ) n.setChildMirrrorBit(2, j);
				}
			}
		}
	}

	if (!internalCall) _stats.toSDAGTime = _clock.elapsed();
	_stats.nNodesSDAG = _nNodes;
	_state = S_SDAG;
	
#if 0 // DEGUG HOW MUCH MIRRORED NODES THERE ARE
	for (int lev = 0; lev < _maxLevel; ++lev) {
		int sCount = 0;
		for (int i = 0; i < _svoLevelsHDD[lev]->size(); ++i) {
			BuildNode bn = _svoLevelsHDD[lev]->item(i);
			if (bn.mirroredX != 0 || bn.mirroredY != 0 || bn.mirroredZ != 0) sCount++;
		}
		printf("LEVEL %d :\t %d nodes with some symm flag\n", lev, sCount);
	}
#endif
	if(!internalCall) printf("OK! [%s]\n", sl::human_readable_duration(_stats.toSDAGTime).c_str());
}


void GeomOctree::toSDAGCanonical() {

	printf("* Transforming DAG -> SDAG (canonical)... "); fflush(stdout);

	struct MirroredNode {
		MirroredNode() : mirrorX(false), mirrorY(false), mirrorZ(false), id(0) {}
		MirroredNode(bool mX, bool mY, bool mZ, id_t id) :
			mirrorX(mX), mirrorY(mY), mirrorZ(mZ), id(id) {}
		bool mirrorX, mirrorY, mirrorZ;
		id_t id;
	};

	std::vector<MirroredNode> correspondences;
	std::map<Node, id_t> uniqueNodesChecker;
	std::vector<Node> uniqueNodes;

	_nNodes = 0;

	_clock.restart();

	for (unsigned int lev = _levels - 1; lev > 0; lev--) {
		unsigned int oldLevSize = (unsigned int)_data[lev].size();
		uniqueNodes.clear();
		uniqueNodesChecker.clear();
		correspondences.resize(oldLevSize);

		for (id_t i = 0; i < _data[lev].size(); i++) {

			Node n = _data[lev][i];
			if (!n.hasChildren()) continue; // skip empty nodes

			// Set invariances
			if (n == n.mirror(true, false, false)) n.setInvariantBit(0);
			if (n == n.mirror(false, true, false)) n.setInvariantBit(1);
			if (n == n.mirror(false, false, true)) n.setInvariantBit(2);

			bool cx, cy, cz;
			Node nC = n.getCanonical(cx, cy, cz);

			// Check children invariances
			if (lev < (_levels - 1)) invertInvs(nC, lev, cx, cy, cz);

			if (uniqueNodesChecker.find(nC) != uniqueNodesChecker.end()) {
				correspondences[i] = MirroredNode(cx, cy, cz, uniqueNodesChecker[nC]);
			} else { // !found
				uniqueNodesChecker[nC] = (id_t)uniqueNodes.size();
				correspondences[i] = MirroredNode(cx, cy, cz, id_t(uniqueNodes.size()));
				uniqueNodes.push_back(nC);
			}
		}
		_data[lev].clear();
		_data[lev] = uniqueNodes;
		_data[lev].shrink_to_fit();
		_nNodes += _data[lev].size();

		for (id_t i = 0; i < _data[lev - 1].size(); i++) {
			Node &n = _data[lev - 1][i];
			for (id_t j = 0; j < 8; j++) {
				if (n.existsChildPointer(j)) {
					MirroredNode mn = correspondences[n.children[j]];
					n.children[j] = mn.id;
					if (mn.mirrorX) n.setChildMirrrorBit(0, j);
					if (mn.mirrorY) n.setChildMirrrorBit(1, j);
					if (mn.mirrorZ) n.setChildMirrrorBit(2, j);
				}
			}
		}
	}

	_stats.toSDAGTime = _clock.elapsed();
	_stats.nNodesSDAG = _nNodes;
	_state = S_SDAG;

	printf("OK! [%s]\n", sl::human_readable_duration(_stats.toSDAGTime).c_str());
}


bool GeomOctree::checkIntegrity() {
	struct QueueItem {
		QueueItem(id_t id, sl::uint8_t l) : nodeID(id), level(l) {}
		id_t nodeID;
		sl::uint8_t level;
	};
	std::queue<QueueItem> queue;

	queue.push(QueueItem(0, 0));

	unsigned int nVisitedNodes = 0;

	while (!queue.empty()) {
		QueueItem qi = queue.front(); queue.pop();
		nVisitedNodes++;
		if (qi.level >= _levels) return false;
		if (qi.nodeID >= _data[qi.level].size()) return false;
		Node &n = _data[qi.level][qi.nodeID];
		if (!n.hasChildren()) return false;
		for (int i = 0; i < 8; ++i) {
			if (qi.level < _levels - 1) {
				if (n.existsChild(i) && !n.existsChildPointer(i)) return false;
				if (!n.existsChild(i) && n.existsChildPointer(i)) return false;
				if (n.existsChild(i)) queue.push(QueueItem(n.children[i], qi.level+1));
			}
		}
	}

	for (unsigned int i = 0; i < _levels; i++) {
		printf("%u\t%zu\n", i, _data[i].size());
	}

	printf("Visited %i nodes\n", nVisitedNodes);

	return true;
}

void GeomOctree::invertInvs(Node &n, int lev, bool inX, bool inY, bool inZ) {
	for (int i = 0; i < 8; ++i) {
		if (n.existsChildPointer(i)) {
			Node & nc = _data[lev + 1][n.children[i]];
			if (inX && nc.getInvariantBit(0)) n.unsetChildMirrrorBit(0, i);
			if (inY && nc.getInvariantBit(1)) n.unsetChildMirrrorBit(1, i);
			if (inZ && nc.getInvariantBit(2)) n.unsetChildMirrrorBit(2, i);
		}
	}
}

size_t GeomOctree::getMemFootprint() {
	size_t mem = sizeof(Octree);
	for (int i = 0; i < _data.size(); ++i) {
		mem += _data[i].capacity()*sizeof(Node);
		mem += sizeof(std::vector<Node>);
	}
	return mem;
}

int GeomOctree::traverse(sl::point3f p) const
{
	if (_state == S_EMPTY) return -1;

	if (!_bbox.contains(p)) return -1;

	if (_state == S_SVO || _state == S_DAG) {

		const Node *n = &_data[0][0];
		sl::point3f nodeCenter = _bbox.center();
		unsigned int nodeLevel = 0;

		while (nodeLevel < _levels) {
			int selectedChild = 4 * int(p[0] > nodeCenter[0]) + 2 * int(p[1] > nodeCenter[1]) + int(p[2] > nodeCenter[2]);
			if (!n->existsChild(selectedChild)) break;
			nodeLevel++;
			float hs = getHalfSide(nodeLevel);
			nodeCenter[0] += (p[0] > nodeCenter[0]) ? hs : -1.f * hs;
			nodeCenter[1] += (p[1] > nodeCenter[1]) ? hs : -1.f * hs;
			nodeCenter[2] += (p[2] > nodeCenter[2]) ? hs : -1.f * hs;
			if (nodeLevel < _levels) n = &_data[nodeLevel][ n->children[selectedChild] ];
		}
		return nodeLevel;
	}

	if (_state == S_SDAG) {

		const Node *n = &_data[0][0];
		sl::point3f nodeCenter = _bbox.center();
		unsigned int nodeLevel = 0;
		bool mX = false, mY = false, mZ = false;
		
		while (nodeLevel < _levels) {
			if (mX) p[0] = 2.f * nodeCenter[0] - p[0];
			if (mY) p[1] = 2.f * nodeCenter[1] - p[1];
			if (mZ) p[2] = 2.f * nodeCenter[2] - p[2];
			int selectedChild = 4 * int(p[0] > nodeCenter[0]) + 2 * int(p[1] > nodeCenter[1]) + int(p[2] > nodeCenter[2]);
			if (!n->existsChild(selectedChild)) break;
			nodeLevel++;
			float hs = getHalfSide(nodeLevel);
			nodeCenter[0] += (p[0] > nodeCenter[0]) ? hs : -1.f * hs;
			nodeCenter[1] += (p[1] > nodeCenter[1]) ? hs : -1.f * hs;
			nodeCenter[2] += (p[2] > nodeCenter[2]) ? hs : -1.f * hs;
			mX = n->getChildMirrrorBit(0, selectedChild);
			mY = n->getChildMirrrorBit(1, selectedChild);
			mZ = n->getChildMirrrorBit(2, selectedChild);

			if (nodeLevel < _levels) n = &_data[nodeLevel][n->children[selectedChild]];
		}
		return nodeLevel;
	}

	return -1;
}

