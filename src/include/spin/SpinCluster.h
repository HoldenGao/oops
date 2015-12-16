#ifndef SPINCLUSTER_H
#define SPINCLUSTER_H

#include <vector>
#include <set>
#include "include/spin/Spin.h"
#include "include/spin/SpinClusterAlgorithm.h"

class cSpinCluster
{
public:
    cSpinCluster(cSpinGrouping * grouping);
    ~cSpinCluster();

    void make();
    CLST_IDX_LIST getClusterIndex(){return _cluster_index_list;};

    friend ostream&  operator << (ostream& outs, const cSpinCluster& clst);
private:
    cSpinGrouping * _grouping;
    CLST_IDX_LIST _cluster_index_list;
};
#endif