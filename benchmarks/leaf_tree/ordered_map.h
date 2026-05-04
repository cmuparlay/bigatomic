#include <functional>
#include <optional>

#include <parlay/parallel.h>
#include <parlay/primitives.h>
#include <parlay/sequence.h>
#include <common/epoch.h>
#include "bigatomic.h"

template <typename K_,
	  typename V_,
	  typename Compare = std::less<K_>>
struct ordered_map {
  using K = K_;
  using V = V_;
  
  // common header for internal nodes and leaves
  struct node {
    K key;
    bool is_leaf;
    bool is_sentinal;
    node(K key, bool is_leaf)
      : key(key), is_leaf(is_leaf), is_sentinal(false) {}
    node(bool is_leaf) :
      is_leaf(is_leaf), is_sentinal(true) {}
  };

  enum Tag {None, Left, Right};
  
  struct LR {
    Tag tagged = None;
    node* left = nullptr;
    node* right = nullptr;
    bool operator==(const LR other) const {
      return tagged == other.tagged && left == other.left && right == other.right;
    }
    LR() {}
    LR(node* left, node* right) : left(left), right(right) {}
    LR(Tag tagged, node* left, node* right) :
      tagged(tagged), left(left), right(right) {}
  };
    
  // internal node
  struct internal : node {
    parlay::big_atomic<LR> children;
    internal(K k, node* left, node* right)
      : node{k,false}, children(LR(left, right)) {}
    internal(node* left) // for the root, only has a left pointer
      : node{false}, children(LR(left, nullptr)) {}
  };

  struct leaf : node {
    V value;
    leaf(K k, V v) : node{k,true}, value(v) {};
    leaf() : node{true} {}; // for the sentinal leaf
  };

  static bool Less(const K& k, leaf* a) {
    return !a->is_sentinal && Compare{}(k, a->key);
  }

  static bool Less(const K& k, internal* a) {
    return a->is_sentinal || Compare{}(k, a->key);
  }

  static bool Equal(const K& k, leaf* a) {
    return !(a->is_sentinal || Compare{}(k, a->key) || Compare{}(a->key ,k));
  }

  internal* root;

  static std::tuple<internal*, internal*, leaf*>
  find_and_fix(internal* root, const K& k) {
    internal* gp = nullptr;
    internal* p = root;
    auto [ignore_tag, pleft, pright] = (p->children).load();
    node* l = pleft;
    while (!l->is_leaf) {
      internal* c = (internal*) l;
      auto [ctag, cleft, cright] = (c->children).load();
      if (ctag != None) {
        l = (ctag == Left) ? cright : cleft;
        LR new_p = Less(k, p) ? LR(l, pright) : LR(pleft, l);
        if (!(p->children).cas(LR(pleft, pright), new_p))
          return find_and_fix(root, k);
      } else {
        gp = p;
        p = c;
        pleft = cleft;
        pright = cright;
        l = Less(k, p) ? pleft : pright;
      }
    }
    return std::make_tuple(gp, p, (leaf*) l);
  }

  bool insert_(const K& k, const V& v) {
    while (true) {
      auto [ignore_gp, p, l] = find_and_fix(root, k);
      if (Equal(k, l)) return false;
      node* new_l = epoch::New<leaf>(k, v);
      node* new_i = (Less(k, l) ?
                     epoch::New<internal>(l->key, new_l, l) :
                     epoch::New<internal>(k, l, new_l));
      auto [tag, left, right] = p->children.load();
      bool lft = Less(k, p);
      if ((lft && p->children.cas(LR(l, right), LR(new_i, right))) ||
          (!lft && p->children.cas(LR(left, l), LR(left, new_i))))
        return true;
      epoch::Delete((leaf*) new_l);
      epoch::Delete((internal*) new_i);
    }
  }

  bool insert(const K& k, const V& v) {
    return epoch::with_epoch([=] {return insert_(k, v);}); }
      
  bool remove_(const K& k) {
    while (true) {
      auto [gp, p, l] = find_and_fix(root, k);
      if (!Equal(k, l)) return false;
      auto [ptag, pleft, pright] = p->children.load();
      bool lft = Less(k,p);
      if ((lft && !p->children.cas(LR(l, pright), LR(Left, l, pright))) ||
          (!lft && !p->children.cas(LR(pleft, l), LR(Right, pleft, l))))
        continue;
      auto [ignore_gptag, gpleft, gpright] = gp->children.load();
      node* c = lft ? pright : pleft;
      bool gplft = Less(k, gp);
      if ((gplft && !gp->children.cas(LR(p, gpright), LR(c, gpright))) ||
          (!gplft && !gp->children.cas(LR(gpleft, p), LR(gpleft, c))))
        find_and_fix(root, k);
      epoch::Retire(p);
      epoch::Retire(l);
      return true;
    }
  }

  bool remove(const K& k) {
    return epoch::with_epoch([=] { return remove_(k);});}

  std::optional<V> find_(const K& k) {
    node* l = root->children.load().left;
    while (!l->is_leaf) {
      internal* ll = (internal*) l;
      l = Less(k,ll) ? ll->children.load().left : ll->children.load().right;
    }
    leaf* ll = (leaf*) l;
    if (Equal(k, ll)) return ll->value;
    else return {};
  }

  std::optional<V> find(const K& k) {
    return epoch::with_epoch([&] { return find_(k);});}

ordered_map() : root(epoch::New<internal>(epoch::New<leaf>())) {}
ordered_map(size_t n) : root(epoch::New<internal>(epoch::New<leaf>())) {}
  ~ordered_map() { Retire(root);}
  
  void print() {
    std::function<void(node*)> prec;
    prec = [&] (node* p) {
      if (p->is_sentinal) std::cout << "[";
      else if (p->is_leaf) std::cout << p->key;
      else {
        internal* pp = (internal*) p;
        auto [tag, left, right] = pp->children.load();
        prec(left);
        if (!left->is_sentinal)
          std::cout << ", ";
        prec(right);
      }
    };
    prec(root->children.load().left);
    std::cout << "]" << std::endl;
  }

  static void Retire(node* p) {
    if (p == nullptr) return;
    if (p->is_leaf) epoch::Retire((leaf*) p);
    else {
      internal* pp = (internal*) p;
      auto [tag, left, right] = pp->children.load();
      parlay::par_do([&] { Retire(left); },
		     [&] { Retire(right); });
      epoch::Retire(pp);
    }
  }
  
  long size(node* p) {
    if (p->is_sentinal) return 0;
    if (p->is_leaf) return 1;
    auto [ignore_tag, left, right] = ((internal*) p)->children.load();
    long lsum, rsum;
    parlay::par_do([&] () { lsum = size(left);},
                   [&] () { rsum = size(right);});
    return lsum + rsum;
  };

  long size() {
    return size(root->children.load().left);
  }
};
