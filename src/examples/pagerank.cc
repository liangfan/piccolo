#include "client.h"
#include "examples/examples.pb.h"

#include <sys/time.h>
#include <sys/resource.h>
#include <algorithm>
#include <libgen.h>

using namespace dsm;
using namespace std;

static float TOTALRANK = 0;
static int NUM_WORKERS = 2;

static const float kPropagationFactor = 0.8;
static const int kBlocksize = 1000;
static const char kTestPrefix[] = "testdata/pr-graph.rec";

DEFINE_bool(build_graph, false, "");
DEFINE_int32(nodes, 10000, "");
DEFINE_string(graph_prefix, kTestPrefix, "Path to web graph.");

// I'd like to use a pair here, but for some reason they fail to count
// as POD types according to C++.  Sigh.
struct PageId {
  uint32_t site;
  uint32_t page;
};

PageId P(int s, int p) {
  PageId pid = { s, p };
  return pid;
}

bool operator==(const PageId& a, const PageId& b) {
  return a.site == b.site && a.page == b.page;
}

namespace std {
static ostream & operator<< (ostream &out, const PageId& p) {
  out << MP(p.site, p.page);
  return out;
}
}

namespace dsm { namespace data {
template<>
  uint32_t hash(PageId p) {
    return hash(p.site) ^ hash(p.page);
  }
}
}

static int SiteSharding(const PageId& p, int nshards) {
  return p.site % nshards;
}

static float powerlaw_random(float dmin, float dmax, float n) {
  float r = (float)random() / RAND_MAX;
  return pow((pow(dmax, n) - pow(dmin, n)) * pow(r, 3) + pow(dmin, n), 1.0/n);
}

static boost::recursive_mutex file_lock;

static vector<int> site_sizes;
static void BuildGraph(int shard, int nshards, int nodes, int density) {
  boost::recursive_mutex::scoped_lock sl(file_lock);

  char* d = strdup(FLAGS_graph_prefix.c_str());
  File::Mkdirs(dirname(d));
  if (site_sizes.empty()) {
    for (int n = 0; n < FLAGS_nodes; ) {
      int c = powerlaw_random(1, (int)(100000. * FLAGS_nodes / 100e6), 0.001);
      site_sizes.push_back(c);
//      LOG_EVERY_N(INFO, 100) << "Site size: " << c;
      n += c;
    }
  }

  string target = StringPrintf("%s-%05d-of-%05d-N%05d", FLAGS_graph_prefix.c_str(), shard, nshards, nodes);
  //FILE* lzo = popen(StringPrintf("lzop -f -q -1 -o%s", target.c_str()).c_str(), "w");

  if (File::Exists(target + ".lzo")) {
    return;
  }

  Page n;
  RecordFile out(target + ".tmp", "w", RecordFile::LZO);
  for (int i = shard; i < site_sizes.size(); i += nshards) {
    for (int j = 0; j < site_sizes[i]; ++j) {
      n.Clear();
      n.set_site(i);
      n.set_id(j);
      for (int k = 0; k < density; k++) {
        int target_site = (random() % 10 != 0) ? i : (random() % site_sizes.size());
        n.add_target_site(target_site);
        n.add_target_id(random() % site_sizes[target_site]);
      }
      out.write(n);
    }
  }

  File::Move(StringPrintf("%s.tmp.lzo", target.c_str()), target + ".lzo");
  //pclose(lzo);
}

static float random_restart_seed() {
  return (1-kPropagationFactor)*(TOTALRANK/FLAGS_nodes);
}

class PRKernel : public DSMKernel {
public:
  int iter;
  vector<Page> nodes;
  TypedGlobalTable<PageId, float>* curr_pr_hash;
  TypedGlobalTable<PageId, float>* next_pr_hash;

  void Init() {
    curr_pr_hash = this->get_table<PageId, float>(0);
    next_pr_hash = this->get_table<PageId, float>(1);
    iter = 0;
  }

  void BuildGraph() {
    srand(0);
    for (int i = 0; i < FLAGS_shards; ++i) {
       ::BuildGraph(i, FLAGS_shards, FLAGS_nodes, 15);
    }
  }

  RecordFile* get_reader() {
    string file = StringPrintf("%s-%05d-of-%05d-N%05d",
        FLAGS_graph_prefix.c_str(), current_shard(), FLAGS_shards, FLAGS_nodes);
    //FILE* lzo = popen(StringPrintf("lzop -d -c %s", file.c_str()).c_str(), "r");
    //RecordFile * r = new RecordFile(lzo, "r");
    return new RecordFile(file, "r", RecordFile::LZO);
  }

  void free_reader(RecordFile* r) {
    //pclose(r->fp.filePointer());
    delete r;
  }

  void Initialize() {
    next_pr_hash->resize((int)(2 * FLAGS_nodes));
    curr_pr_hash->resize((int)(2 * FLAGS_nodes));
  }

  void WriteStatus() {
    fprintf(stderr, "Iteration %d, PR:: ", iter);
    fprintf(stderr, "%.2f\n", curr_pr_hash->get(P(0, 0)));
  }

  void PageRankIter() {
    ++iter;

    Page n;
    Timer t;

    RecordFile *r = get_reader();
    while (r->read(&n)) {
      next_pr_hash->update(P(n.site(), n.id()), random_restart_seed());

      float v = curr_pr_hash->get_local(P(n.site(), n.id()));
      float contribution = kPropagationFactor * v / n.target_site_size();
      for (int i = 0; i < n.target_site_size(); ++i) {
        next_pr_hash->update(P(n.target_site(i), n.target_id(i)), contribution);
      }
    }
    free_reader(r);

    char host[1024];
    gethostname(host, 1024);
    VLOG(1) << "Finished shard " << current_shard() << " on " << host << " in " << t.elapsed();
  }

  void ResetTable() {
    // Move the values computed from the last iteration into the current table.
    swap(curr_pr_hash, next_pr_hash);
    next_pr_hash->clear(current_shard());
  }
};
REGISTER_KERNEL(PRKernel);
REGISTER_METHOD(PRKernel, BuildGraph);
REGISTER_METHOD(PRKernel, Initialize);
REGISTER_METHOD(PRKernel, WriteStatus);
REGISTER_METHOD(PRKernel, PageRankIter);
REGISTER_METHOD(PRKernel, ResetTable);

DEFINE_bool(checkpoint, false, "Checkpoint between iterations.");

int Pagerank(ConfigData& conf) {
  conf.set_slots(256);

  NUM_WORKERS = conf.num_workers();
  TOTALRANK = FLAGS_nodes;

  Registry::create_table<PageId, float>(0, FLAGS_shards, &SiteSharding, &Accumulator<float>::sum);
  Registry::create_table<PageId, float>(1, FLAGS_shards, &SiteSharding, &Accumulator<float>::sum);

  StartWorker(conf);

  Master m(conf);
  if (FLAGS_build_graph) {
    vector<int> shards;
    for (int i = 0; i < conf.num_workers(); ++i) {
      shards.push_back(i);
    }

    RUN_RANGE(m, PRKernel, BuildGraph, 0, shards);
    return 0;
  }


  int i = 0;
  ParamMap *pmap = NULL;

  if (FLAGS_checkpoint) {
    pmap = m.restore();
  }

  if (pmap == NULL) {
    i = 0;
    pmap = new ParamMap;
    RUN_ALL(m, PRKernel, Initialize, 0);
  } else {
    i = pmap->get_int("iteration");
    LOG(INFO) << "Restoring pagerank at iteration: " << i;
  }

  for (; i < FLAGS_iterations; i++) {
    Master::RunDescriptor r = Master::RunDescriptor::Create("PRKernel", "PageRankIter", 0);
    pmap->set_int("iteration", i);
    if (FLAGS_checkpoint) {
      r.checkpoint_type = CP_MASTER_CONTROLLED;
      // We only need to save the next_pr table, which alternates each iteration.
      r.checkpoint_tables = MakeVector((i % 2 == 0) ? 1 : 0);
    } else {
      r.checkpoint_type = CP_NONE;
    }
    r.params = pmap->to_params();

    m.run_all(r);
    RUN_ALL(m, PRKernel, ResetTable, 0);
    RUN_ONE(m, PRKernel, WriteStatus, 0);
  }

  return 0;
}
REGISTER_RUNNER(Pagerank);
