#ifndef mpi_backend_h
#define mpi_backend_h

#include "darma_config.h"
#if DARMA_DEBUG_PRINT
#include <fmt/format.h>
#define darmaDebug(flag, fmtstr, ...) \
  if (DebugFlag<DebugFlags::flag>::active) \
    std::cout << fmt::format(fmtstr, __VA_ARGS__) << std::endl
#else
#define darmaDebug(...)
#endif

#include <sys/time.h>

struct DebugFlags {
  struct LB {};
  struct SendRecv {};
  struct Interop {};
  struct Task {};
};

template <class T>
struct DebugFlag {
  static bool active;
};
template <class T> bool DebugFlag<T>::active = false;

#include "mpi_async_ref.h"
#include "mpi_task.h"
#include "mpi_send_recv.h"
#include "mpi_phase.h"
#include "mpi_predicate.h"
#include "mpi_pending_recv.h"
#include "gather.h"
#include "broadcast.h"


#include <darma/serialization/simple_handler.h>
#include <darma/serialization/serializers/all.h>

#include <mpi.h>
#include <list>
#include <vector>
#include <map>
#include <set>

template <class Accessor, class T, class Index>
int recv_task_id();

static inline uint64_t rdtsc(void)
{
  uint32_t hi, lo;
  __asm__ __volatile__ ("rdtsc" : "=a"(lo), "=d"(hi));
  return uint64_t( (uint64_t)lo | (uint64_t)hi<<32);
}

struct MpiBackend {
  struct migration {
    int index;
    void* buf;
    void* obj;
    int size;
    int rank;
    int mpiParent;
    migration(int i, void* b, void* o, int s, int r, int p) :
      index(i), buf(b), obj(o), size(s), rank(r), mpiParent(p)
    {
    }
  };

  typedef enum {
    RandomLB,
    CommSplitLB,
    ZoltanLB,
    DebugLB
  } lb_type_t;

  struct PerfCtrReduce {
    uint64_t total;
    uint64_t max;
    uint64_t min;
    uint64_t maxTask;
    uint64_t minTask;
    uint64_t maxLocalTasks;
  };

  using Context=Frontend<MpiBackend>;
  using task=TaskBase<Context>;

  static constexpr uintptr_t REQUEST_CLEAR = 0x1;

  MpiBackend(MPI_Comm comm, int argc, char** argv);

  ~MpiBackend();

  void error(const char* fmt, ...);

  Context& frontend() {
    return *static_cast<Context*>(this);
  }

  Context* frontendPtr() {
    return static_cast<Context*>(this);
  }

  template <class T, class... Args>
  auto make_async_ref(Args&&... args){
    return async_ref<T,Modify,Modify>::make(std::forward<Args>(args)...);
  }

  template <class T, class Idx>
  auto make_collection(Idx size){
    auto ret = async_ref<collection<T,Idx>,None,Modify>::make(size);
    ret->setId(collIdCtr_++);
    collections_[ret->id()] = ret.get();
    return ret;
  }

  double get_time() const {
    timeval t; gettimeofday(&t, nullptr);
    return t.tv_sec + 1e-6*t.tv_usec;
  }

  template <class T, class Idx>
  auto make_phase(mpi_collection_ptr<T,Idx>& coll){
    Phase<Idx> ret(coll->size());
    std::vector<int> local;
    for (auto& pair : coll->localElements()){
      local.push_back(pair.first);
      ret->local_.emplace_back(pair.first);
    }
    make_global_mapping_from_local(coll->size(), local, ret->index_to_rank_mapping_);
    return ret;
  }

  template <class Idx>
  auto make_phase(Idx idx){
    Phase<Idx> ret(idx);
    local_init_phase(ret);
    return ret;
  }

  template <class FrontendTask> 
  auto allocate_task(FrontendTask&& task){
    return new Task<Context,FrontendTask>(std::move(task));
  }

  template <class ReduceFunctor>
  auto allocate_reduce_task(){
  }

  template <class PredicateOp, class DependentOp>
  auto allocate_predicate_task(PredicateOp&& pred_op, DependentOp&& dep_op){
    return new PredicateTask<PredicateOp,DependentOp,Context>(std::move(pred_op), std::move(dep_op));
  }

  void register_dependency(task* t, mpi_async_ref& in);

  bool run_root() const {
    return true;
  }

  bool is_root() const {
    return rank_ == 0;
  }

  void run_worker(){}

  template <class Idx>
  void rebalance(Phase<Idx>& ph){
    clear_tasks();
    MPI_Barrier(comm_); //bad to do, but for the timers
    std::vector<pair64> newConfig = balance(ph->local());
    reset_phase(newConfig, ph->local_, ph->index_to_rank_mapping_);
  }

  template <class T>
  void register_dependency(task*, T&&){
    //don't register dependencies that aren't async_refs
  }
  
  template <class T> //no ops if not async refs
  void register_pred_cond_dependency(task*, T&&){}

  void register_pred_cond_dependency(task* t, mpi_async_ref& in){
    register_dependency(std::move(t),in);
  }

  template <class T> //no ops if not async refs
  void register_pred_body_dependency(task* t, T&&){}

  void register_pred_body_dependency(task* t, mpi_async_ref& in){
    register_dependency(std::move(t),in);
  }

  void register_control_task(task* t){
    if (t->join_counter() == 0) taskQueue_.push_back(std::move(t));
    clear_tasks();
  }

  void register_task(task* t){
    if (t->join_counter() == 0) taskQueue_.push_back(std::move(t));
  }

  void register_predicated_task(task* t){
    //don't do anything special for predicate tasks
    register_task(std::move(t));
    clear_tasks();
  }

  //template <class PackFunctor, class UnpackFunctor, class TaskFunctor,
  //          template <class> Ref, class T, class Index, class... Args>
  //auto make_active_send_op(Ref<T>&& ref, idempotent_task_base<T>& acc, Index&& idx, Args&&... args){
  //  SendOp<Ref,T> op(std::move(ref));
  //  //MPI_Isend(..., op.getArgument().allocateRequest());
  //  return op;
  //}

  template <class Accessor, class T, class Index>
  void from_mpi_shuffle(mpi_collection_ptr<T,Index>& mpi_coll, collection<T,Index>* coll){
    async_ref_base<T>* dummy;
    using pack_buf_t = decltype(make_packed_buffer<Accessor>(non_local_handler_t{}, *dummy));
    std::vector<migration> toSend;
    std::vector<migration> toRecv;
    std::vector<pack_buf_t> packers;
    for (auto& pair : mpi_coll->localElements()){
      int index = pair.first;
      int newLoc = coll->getRank(index);
      if (newLoc != rank_){
        darmaDebug(Interop, "Rank={} needs to send DARMA {} back to {}", rank_, index, newLoc);
        non_local_handler_t handler{};
        auto elem = pair.second;
        auto s_ar = handler.make_sizing_archive();
        Accessor::compute_size(*elem, s_ar);
        auto p_ar = handler.make_packing_archive(std::move(s_ar));
        Accessor::pack(*elem, p_ar);
        auto buffer = handler.extract_buffer(std::move(p_ar));
        toSend.emplace_back(index, buffer.data(), elem.get(), buffer.capacity(), newLoc, rank_);
        packers.emplace_back(std::move(buffer));
      }
    }

    for (auto& pair : coll->localElements()){
      int index = pair.first;
      auto elem = pair.second;
      int oldLoc = coll->getParentMpiRank(index);
      if (oldLoc != rank_){
        darmaDebug(Interop, "Rank={} needs to recv DARMA {} back from {}", rank_, index, oldLoc);
        toRecv.emplace_back(index, nullptr, elem.get(), 0, oldLoc, oldLoc);
      }
    }

    rebalance(toSend, toRecv);

    for (migration& m : toRecv){
      non_local_handler_t handler{};
      auto u_ar = handler.make_unpacking_archive(
        darma::serialization::NonOwningSerializationBuffer(m.buf, m.size));
      T* obj = (T*) m.obj;
      Accessor::unpack(*obj, u_ar);
      free_temp_buffer(m.buf, m.size);
    }
  }

  //todo - make this a set of variadic args
  //todo - have the frontend do most of the work for this
  template <class Accessor, class T, class Index>
  auto from_mpi(mpi_collection_ptr<T,Index>&& mpi_coll){
    //no load balancing yet, so this does nothing
    if (mpi_coll->referencesDarmaCollection()){
      auto darma_coll = mpi_coll->darmaCollection();
      from_mpi_shuffle<Accessor,T,Index>(mpi_coll, darma_coll.get());
      //this was remapped from a previous collection
      darma_coll->assignMpi(std::move(mpi_coll));
      return async_ref<collection<T,Index>,None,Modify>::make(darma_coll);
    } else {
      //no collection ever existed, so better make it now
      auto ref = async_ref<collection<T,Index>,None,Modify>
            ::make(rank_, mpi_coll->size(), mpi_coll->localElements());
      ref->setId(collIdCtr_++);
      ref->assignMpi(std::move(mpi_coll));
      return ref;
    }
  }

  template <class Accessor, class T, class Index>
  auto to_mpi(async_ref_base<collection<T,Index>>&& arg){
    //this is a fully blocking call
    clear_tasks();
    if (!arg->hasMpiParent())
      error("darma collection cannot return an MPI collection if no MPI collection was originally moved in");

    auto&& mpiParent = arg->moveMpiParent();
    async_ref_base<T>* dummy;
    using pack_buf_t = decltype(make_packed_buffer<Accessor>(non_local_handler_t{}, *dummy));
    std::vector<migration> toSend;
    std::vector<migration> toRecv;
    std::vector<pack_buf_t> packers;
    for (auto& pair : arg->localElements()){
      int index = pair.first;
      int newLoc = arg->getParentMpiRank(index);
      if (newLoc != rank_){
        darmaDebug(Interop, "Rank={} needs to send {} back to {}", rank_, index, newLoc);
        non_local_handler_t handler{};
        auto elem = pair.second;
        auto s_ar = handler.make_sizing_archive();
        Accessor::compute_size(*elem, s_ar);
        auto p_ar = handler.make_packing_archive(std::move(s_ar));
        Accessor::pack(*elem, p_ar);
        auto buffer = handler.extract_buffer(std::move(p_ar));
        toSend.emplace_back(index, buffer.data(), elem.get(), buffer.capacity(), newLoc, newLoc);
        packers.emplace_back(std::move(buffer));
      }
    }
    for (auto& pair : mpiParent->localElements()){
      int index = pair.first;
      auto elem = pair.second;
      int oldLoc = arg->getRank(index);
      if (oldLoc != rank_){
        darmaDebug(Interop, "Rank={} needs to recv {} back from {}", rank_, index, oldLoc);
        toRecv.emplace_back(index, nullptr, elem.get(), 0, oldLoc, rank_);
      }
    }

    rebalance(toSend, toRecv);

    for (migration& m : toRecv){
      non_local_handler_t handler{};
      auto u_ar = handler.make_unpacking_archive(
        darma::serialization::NonOwningSerializationBuffer(m.buf, m.size));
      T* obj = (T*) m.obj;
      Accessor::unpack(*obj, u_ar);
      free_temp_buffer(m.buf, m.size);
    }

    mpiParent->setDarmaCollection(arg.sharedPtr());
    return std::move(mpiParent);
  }

  // Note: if you want to allocate the buffer using a custom allocator, make it
  //       the template parameter of this type. (If you need a stateful allocator,
  //       talk to me and I'll add that).
  using non_local_handler_t = darma::serialization::SimpleSerializationHandler<>;
  // TODO update the type of local_handler_t once the copy_constructor_archive example
  //      gets moved to a header file in DARMA serialization and organized into a handler
  using local_handler_t = darma::serialization::SimpleSerializationHandler<>;

  int makeUniqueTag(int collId, int dstId, int srcId, int taskId = 0 /*zero means no task*/);

  template <class Accessor, class T, class LocalIndex, class RemoteIndex, class... Args>
  auto make_send_op(async_ref_base<T>&& ref,
                    LocalIndex&& local, RemoteIndex&& remote,
                    Args&&... args){
    using index_t = std::decay_t<LocalIndex>;
    if (!ref.hasParent()){
      error("sending object with no parent collection");
    }

    auto* parent = ref.template getParent<index_t>();
    auto& dst = parent->getIndexInfo(remote);
    auto& src = parent->getIndexInfo(local);

    bool is_local = false; //push everything through MPI for now
    if(is_local) {
      //extra work needed here to put a local listener in the list
    } else {
      // The templated methods below operate on an instance, in case you need
      // something like a stateful allocator at some point in the future.
      // (All SerializationHandlers that are currently implemented, though,
      // use static methods for everything).
      auto buffer = make_packed_buffer<Accessor>(
        non_local_handler_t{}, ref,
        std::forward<LocalIndex>(local),
        std::forward<RemoteIndex>(remote),
        std::forward<Args>(args)...
      );
      int reqId = send_data(ref, parent->id(), src, dst, buffer.data(), buffer.capacity());
      auto* listener = new PendingSend<decltype(buffer)>(std::move(buffer));
      listener->increment_join_counter();
      listeners_[reqId] = listener;
    }

    //size
    //allocate a send buffer
    //post the MPI request with a tag from att
    SendOp<T> op(std::move(ref));
    return op;
  }

  template <class Accessor, class SerializationHandler,
            class T, class LocalIndex, class RemoteIndex, class... Args>
  auto make_packed_buffer(SerializationHandler&& handler,
                          async_ref_base<T>& ref,
                          LocalIndex&& local, RemoteIndex&& remote,
                          Args&&... args){
    auto s_ar = handler.make_sizing_archive();
    // TODO pass idx to the Accessor (if that's part of the concept?)
    Accessor::compute_size(*ref, local, remote, s_ar, std::forward<Args>(args)...);
    auto p_ar = handler.make_packing_archive(std::move(s_ar));
    // TODO forward idx to the Accessor (if that's part of the concept?)
    Accessor::pack(*ref, local, remote, p_ar, std::forward<Args>(args)...);
    return std::forward<SerializationHandler>(handler).extract_buffer(std::move(p_ar));
  }

  template <class Accessor, class SerializationHandler, class T, class... Args>
  auto make_packed_buffer(SerializationHandler&& handler,
                          async_ref_base<T>& ref, Args&&... args){
    auto s_ar = handler.make_sizing_archive();
    // TODO pass idx to the Accessor (if that's part of the concept?)
    Accessor::compute_size(*ref, s_ar, std::forward<Args>(args)...);
    auto p_ar = handler.make_packing_archive(std::move(s_ar));
    // TODO forward idx to the Accessor (if that's part of the concept?)
    Accessor::pack(*ref, p_ar, std::forward<Args>(args)...);
    return std::forward<SerializationHandler>(handler).extract_buffer(std::move(p_ar));
  }


  template <class Accessor, class T, class Index, class... Args>
  auto make_active_send_op(async_ref_base<T>&& ref, Index&& idx, Args&&... args){
    using index_t = std::decay_t<LocalIndex>;
    if (!ref.hasParent()){
      error("sending object with no parent collection");
    }

    auto* parent = ref.template getParent<index_t>();
    auto& dst = parent->getIndexInfo(idx);
    bool is_local = false; //push everything through MPI for now
    if(is_local) {
      //extra work needed here to put a local listener in the list
    } else {
      // The templated methods below operate on an instance, in case you need
      // something like a stateful allocator at some point in the future.
      // (All SerializationHandlers that are currently implemented, though,
      // use static methods for everything).
      auto buffer = make_packed_buffer<Accessor>(non_local_handler_t{}, ref,
                                                 std::forward<Args>(args)...);
      IndexInfo src; //the source doesn't actuall matter here
      src.rank = rank_;
      src.rankUniqueId = 0;
      send_data(ref, parent->id(), src, dst, buffer.data(), buffer.capacity(),
                recv_task_id<Accessor,T,Index>());
    }

    //size
    //allocate a send buffer
    //post the MPI request with a tag from att
    SendOp<T> op(std::move(ref));
    return op;
  }

  template <class Accessor, class T, class LocalIndex, class RemoteIndex, class... Args>
  auto make_recv_op(async_ref_base<T>&& ref, LocalIndex&& local, RemoteIndex&& remote, Args&&... args){
    using index_t = std::decay_t<LocalIndex>;
    auto* parent = ref.template getParent<index_t>();
    auto& localEntry = parent->getIndexInfo(local);
    auto& remoteEntry = parent->getIndexInfo(remote);

    using MyRecv = NonLocalPendingRecv<Accessor,T,index_t,std::remove_reference_t<Args>...>;
    auto* pending = new MyRecv(std::move(ref), std::forward<Args>(args)...);
    pending->increment_join_counter();
    add_pending_recv(pending, parent->id(), localEntry, remoteEntry);
    RecvOp<T> op{};
    return op;
  }

  void rebalance(std::vector<migration>& objToSend,
                 std::vector<migration>& objToRecv);

  template <class Accessor, class Index, class T>
  auto rebalance(Phase<Index>& ph, async_collection<T,Index>&& coll){
    clear_tasks();
    double t_start = get_time();
    int numSends = 0;
    int numRecvs = 0;
    async_ref_base<T>* dummy;
    using pack_buf_t = decltype(make_packed_buffer<Accessor>(non_local_handler_t{}, *dummy));
    std::vector<migration> toSend;
    std::vector<migration> toRecv;
    std::vector<pack_buf_t> packers;
    for (auto& pair : coll->localElements()){
      int index = pair.first;
      int newLoc = ph->getRank(index);
      if (newLoc != rank_){
        ++numSends;
        non_local_handler_t handler{};
        auto elem = pair.second;
        auto s_ar = handler.make_sizing_archive();
        Accessor::compute_size(*elem, s_ar);
        auto p_ar = handler.make_packing_archive(std::move(s_ar));
        Accessor::pack(*elem, p_ar);
        auto buffer = handler.extract_buffer(std::move(p_ar));
        int mpiParent = coll->getParentMpiRank(index);
        toSend.emplace_back(index, buffer.data(), elem.get(),
                            buffer.capacity(), newLoc, mpiParent);
        packers.emplace_back(std::move(buffer));
      }
    }

    for (const LocalIndex& lidx : ph->local()){
      int oldLoc = coll->getRank(lidx.index);
      if (oldLoc != rank_){
        auto newT = coll->emplaceNew(lidx.index);
        toRecv.emplace_back(lidx.index, nullptr, newT.get(), 0, oldLoc, -1);
        ++numRecvs;
      }
    }

    rebalance(toSend, toRecv);

    for (migration& m : toRecv){
      non_local_handler_t handler{};
      auto u_ar = handler.make_unpacking_archive(
        darma::serialization::NonOwningSerializationBuffer(m.buf, m.size));
      T* obj = (T*) m.obj;
      Accessor::unpack(*obj, u_ar);
      free_temp_buffer(m.buf, m.size);
      coll->addParentMpiRank(m.index, m.mpiParent);
    }

    for (migration& m : toSend){
      coll->remove(m.index);
      coll->removeParentMpiRank(m.index);
    }

    coll->index_mapping_ = ph->index_to_rank_mapping_;

    double t_stop = get_time();
    double t_ms = (t_stop - t_start)*1e3;
    if (rank_ == 0){
      std::cout << "Load balance data migration took " << t_ms << "ms" << std::endl;
    }


    async_collection<T,Index> ret(std::move(coll));
    return ret;
  }

  template <class SendOp>
  auto register_send_op(SendOp&& op){
    //already done
  }

  template <class SendOp>
  auto register_active_send_op(SendOp&& op){
    //already done
  }

  template <class RecvOp>
  auto register_recv_op(RecvOp&& op){
    //already done
  }

  template <class Phase, class GeneratorTask>
  void register_phase_collection(Phase& ph, GeneratorTask&& gen){
    clear_tasks();
    int size = ph->local().size();
    for (auto iter=ph->index_begin(); iter != ph->index_end(); ++iter){
      auto& local = *iter;
      auto* be_task = gen.generate(static_cast<Context*>(this),local.index);
      //these rigorously cannot have any dependencies
      //frontend().register_dependencies(be_task);
      be_task->setCounters(&local.counters);
      taskQueue_.push_back(be_task);
    }
    //flush all tasks created by this collection
    //run "bulk-synchronously" for now
    clear_tasks();
  }

  template <class Phase, class Terminator, class GeneratorTask>
  void register_phase_idempotent_collection(Phase& ph, Terminator&& term, GeneratorTask&& gen){
    register_phase_collection(ph, std::move(gen));
    //add a task that will keep looping until termination detection is achieved
    //taskQueue_.push_back(terminate_task);
  }

  template <class Functor, class T, class Idx>
  auto register_local_reduce(async_ref_base<collection<T,Idx>>&& collIn,
                             async_ref_base<collection<T,Idx>>& collOut)
  {
    auto& coll = *collIn;
    auto identity = Functor::identity();
    for (auto iter=coll.localElements().begin(); iter != coll.localElements().end(); ++iter){
      auto& contrib = iter->second;
      Functor()(*contrib, identity);
    }
    return async_ref_base<decltype(identity)>(in_place_construct, std::move(identity));
  }

  template <class Functor, class T, class Idx>
  auto register_reduce(async_ref_base<collection<T,Idx>>&& collIn,
                       async_ref_base<collection<T,Idx>>& collOut)
  {
    clear_tasks();

    auto localResult = register_local_reduce<Functor>(std::move(collIn), collOut);
    MPI_Allreduce(MPI_IN_PLACE,
                  Functor::mpiBuffer(*localResult),
                  Functor::mpiSize(*localResult),
                  Functor::mpiType(*localResult),
                  Functor::mpiOp(*localResult),
                  comm_);
    return localResult;
  }
  
  template <class Phase, class T, class Idx>
  auto register_phase_gather(Phase& ph, int root,
                                        async_ref_base<collection<T, Idx>>&& coll_in)
  {
    // Finish all pending tasks
    clear_tasks();
    return darma_backend::gather(std::move(coll_in), root, comm_);
  }
  
  template <class Idx, class Phase, class T>
  auto register_phase_broadcast(Phase& ph, int root, 
                                async_ref_base<T>&& ref_in)
  {
    // Finish all pending tasks
    clear_tasks();
    return darma_backend::broadcast<Idx>(std::move(ref_in), root, comm_);
  }

  template <class T, class Index>
  auto make_local_collection(Index size){
    return std::make_unique<mpi_collection<T,Index>>(size);
  }

  template <class T>
  auto get_collection_element(int id, int idx){
    //hope this is an int
    collection<T,int>* coll = static_cast<collection<T,int>*>(collections_[idx]);
    return get_element(idx, coll);
  }

  template <class Index, class T>
  auto get_element(const Index& idx, collection<T,Index>* coll){
    auto t = coll->getElement(idx);
    if (t){
      async_ref_base<T> ret(coll->getElement(idx));
      ret.setParent(coll);
      return ret;
    } else if (!coll->initialized()){
      async_ref_base<T> ret = async_ref_base<T>::make();
      coll->setElement(idx, ret.sharedPtr());
      ret.setParent(coll);
      return ret;
    } else {
      error("do not yet support remote get_element from collections: index %d on rank %d", idx, rank_);
      return async_ref_base<T>::make();
    }
  }

  template <class Index, class T>
  auto get_element(const Index& idx, async_ref_base<collection<T,Index>>& coll){
    return get_element<Index,T>(idx, coll.get());
  }

  template <class Op, class T, class U>
  void sequence(Op&& op, T&& t, U&& u){}

  template <class Op, class T, class Index>
  void sequence(Op&& op,
                async_ref_base<collection<T,Index>>& closure,
                async_ref_base<collection<T,Index>>& continuation){
    continuation->setInitialized();
  }

  template <class Accessor, class T, class Index>
  static int register_recv_generator(){
    int id = taskIdCtr_++;
    generators_.push_back(new RecvOpGenerator<Context,Accessor,T,Index>);
    return id;
  }

  void* allocate_temp_buffer(int size);
  void free_temp_buffer(void* buf, int size);
  
  void flush()
  {
    clear_tasks();
  }

 private:
  using pair64 = std::pair<uint64_t,uint64_t>;
  struct sortByWeight {
    bool operator()(const pair64& lhs, const pair64& rhs) const {
      return lhs.first < rhs.first;
    }
  };

  void inform_listener(int idx);

  /**
   * @brief progress_dependencies
   * @return Whether there are any pending active requests (non-null)
   */
  bool progress_dependencies();
  void progress_tasks();
  void progress_engine();
  void clear_dependencies();
  void clear_tasks();
  void clear_queues();
  void make_global_mapping_from_local(int total_size, const std::vector<int>& local,
                                      std::vector<IndexInfo>& mapping);
  void make_rank_mapping(int total_size, std::vector<IndexInfo>& mapping, std::vector<int>& local);
  int allocate_request();
  void create_pending_recvs();
  void add_pending_recv(PendingRecvBase* recv, int collId,
                        const IndexInfo& local, const IndexInfo& remote);
  int send_data(mpi_async_ref& in, int collId,
                 const IndexInfo& src, const IndexInfo& dst,
                 void* data, int size, int taskId = 0 /*zero means no task*/);

  void send_data(int dest, void* data, int size, int tag, MPI_Request* req);
  void recv_data(int src, void* data, int size, int tag, MPI_Request* req);

  void reset_phase(const std::vector<pair64>& config,
                   std::vector<LocalIndex>& local,
                   std::vector<IndexInfo>& indices);

  template <class Index>
  void local_init_phase(Phase<Index>& ph){
    std::vector<int> localIndices;
    make_rank_mapping(ph->size_, ph->index_to_rank_mapping_, localIndices);
    for (int idx : localIndices){
      ph->local_.emplace_back(idx);
    }
  }

  /**
   * @brief tradeTasks Try to find two tasks to trade between ranks that have a given difference
   * in workload size. Note, the desired delta is 1/2 the difference in total workload since
   * once subtracts and the other adds
   * @param desiredDelta The difference between big/small tasks wanted for exact match
   * @param bigger  The list of task sizes on the rank with bigger workload
   * @param smaller The list of task sizes on the rank with smaller workload
   * @param biggerIdx The index of the task in bigger list that best satisfies desired delta
   * @param smallerIdx The index of the task in small list that best satisfies desired delta
   * @return The actual delta achieved
   */
  uint64_t tradeTasks(uint64_t desiredDelta,
                  const std::vector<pair64>& bigger,
                  const std::vector<pair64>& smaller,
                  std::vector<int>& biggerIdx, 
                  std::vector<int>& smallerIdx,
                  int maxTrades);

  uint64_t tradeTasks(uint64_t desiredDelta,
                  uint64_t maxOverage,
                  const std::vector<pair64>& bigger,
                  const std::vector<pair64>& smaller,
                  int biggerIdx, int smallerIdx);

  /**
   * @brief takeTasks Try to find a set of tasks to balance workload between two rans.
   * Note, the desired delta is 1/2 the difference in total workload.
   * @param desiredDelta The difference between big/small tasks wanted for exact match
   * @param giver The list of task sizes on the rank with bigger workload
   * @return The set of tasks that best matches delta
   */
  std::set<int> takeTasks(uint64_t desiredDelta, const std::vector<pair64>& giver, int maxTake);

  /**
   * @brief balance
   * @param local
   * @return The new local configuraiton
   */
  std::vector<pair64> balance(const std::vector<LocalIndex>& local);

  /**
   * @brief balance
   * @param localConfig
   * @return The new local configuraiton
   */
  std::vector<pair64> balance(std::vector<pair64>&& localConfig);

  /**
   * @brief runCommSplitBalancer
   * @param localConfig
   * @param newLocalConfig in-out return of new local config after moving tasks
   * @param localWork The total amount of work currently local
   * @param globalWork  The total amount of work globally
   * @param maxNumLocalTasks  The max number of tasks on any given node
   * @param allowGiveTake whether to rigorously enforce only "exchanging" tasks
   *         or to allow giving/taking tasks that change num local
   */
  void runCommSplitBalancer(std::vector<pair64>&& localConfig,
      std::vector<pair64>& newLocalConfig,
      uint64_t localWork, uint64_t globalWork,
      int maxNumLocalTasks, bool allowTrades, bool allowGiveTake);

  /**
   * @brief getPartner Get a partner for trading tasks
   * @param rank Current rank in relevant communicator
   * @return Your partner in the trade
   */
  int getTradingPartner(int rank) const;

  std::vector<pair64> zoltanBalance(std::vector<pair64>&& localConfig);
  std::vector<pair64> randomBalance(std::vector<pair64>&& localConfig);
  std::vector<pair64> debugBalance(std::vector<pair64>&& localConfig);
  std::vector<pair64> commSplitBalance(std::vector<pair64>&& localConfig);

 private:
  struct PostedRecv {
    int size;
    void* data;
    int id;
    PostedRecv(int i, int s, void* d) :
      id(i), size(s), data(d){}
  };

  std::vector<Listener*> listeners_;
  std::vector<int> indices_;
  std::vector<MPI_Request> requests_;
  std::vector<MPI_Status> statuses_;
  std::vector<int> freeRequests_;
  std::vector<darma::serialization::DynamicSerializationBuffer<>> sendBuffers_;
  std::list<task*> taskQueue_;
  std::map<int,collection_base*> collections_;
  std::map<int,std::list<PostedRecv>> recvsQueued_;
  std::map<int,std::map<int,std::list<PendingRecvBase*>>> pendingRecvs_;
  MPI_Comm comm_;
  int rank_;
  int size_;
  int collIdCtr_;
  int numPendingProbes_;
  static int taskIdCtr_;
  static std::vector<RecvOpGeneratorBase<Context>*> generators_;

  //for idempotent task regions
  int activeWindow_;

  MPI_Op perfCtrOp_;
  MPI_Datatype perfCtrType_;

  lb_type_t lbType_;

};

template <class Accessor, class T, class Index>
int recv_task_id(){
  return MpiBackend::register_recv_generator<Accessor,T,Index>();
}

static inline auto allocate_context(MPI_Comm comm, int argc, char** argv){
  return std::make_unique<Frontend<MpiBackend>>(comm, argc, argv);
}

#endif

