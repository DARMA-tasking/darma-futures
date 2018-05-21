#ifndef mpi_be_collection_h
#define mpi_be_collection_h

#include <map>
#include <vector>
#include "mpi_phase.h"

template <class Idx>
struct Linearization {};

template <>
struct Linearization<int> {
  int toLinear(int rank, int size){
    return rank;
  }
  int fromLinear(int rank, int size){
    return rank;
  }
};

struct collection_base {
  void setId(int id){
    id_ = id;
  }
  
  int id() const {
    return id_;
  }

 private:
  int id_;
};

template <class T, class Idx>
struct collection : public collection_base {
  collection(int size) : size_(size),  initialized_(false) {}

  T* getElement(int idx){
    auto iter = local_elements_.find(idx);
    return iter == local_elements_.end() ? nullptr : iter->second;
  }

  void setElement(int idx, T* t){
    local_elements_[idx] = t;
  }

  int size() const {
    return size_;
  }

  int getRank(int index){
    return index_mapping_[index].rank;
  }

  bool initialized() const {
    return initialized_;
  }

  void setInitializ() {
    initialized_ = true;
  }

  void initPhase(Phase<int>& ph){
    index_mapping_ = ph.mapping();
  }

  const IndexInfo& getIndexInfo(int index){
    return index_mapping_[index];
  }

  std::vector<IndexInfo> index_mapping_;
  std::map<int, T*> local_elements_;
  int size_;
  bool initialized_;

};


template <class T, class Idx>
struct mpi_collection {

  mpi_collection(int size) : coll_(size) {}

  mpi_collection(collection<T,Idx>&& coll) : coll_(std::move(coll)) {}

  collection<T,Idx>& getCollection(){
    return coll_;
  }

 private:
  collection<T,Idx> coll_;

};
#endif

