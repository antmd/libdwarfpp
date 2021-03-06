/* dwarfpp: C++ binding for a useful subset of libdwarf, plus extra goodies.
 * 
 * lib.hpp: basic C++ wrapping of libdwarf C API.
 *
 * Copyright (c) 2008--12, Stephen Kell.
 */

#ifndef DWARFPP_LIB_HPP_
#define DWARFPP_LIB_HPP_

#include <iostream>
#include <utility>
#include <memory>
#include <stack>
#include <vector>
#include <queue>
#include <cassert>
#include <boost/optional.hpp>
#include <boost/icl/interval_map.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>
#include <boost/iterator/iterator_facade.hpp>
#include <boost/iterator/filter_iterator.hpp>
#include <srk31/selective_iterator.hpp>
#include <boost/iterator/transform_iterator.hpp>
#include "spec.hpp"
#include "opt.hpp"
#include "attr.hpp" // includes forward decls for iterator_df!
#include "expr.hpp"

namespace srk31 {
    template<class Iter, class Pred> using selective_iterator =
    ::selective_iterator<Iter, Pred>;
    
}
namespace dwarf
{
	using std::string;
	using std::vector;
	using std::stack;
	using std::endl;
	using std::ostream;
	using std::cerr;

	// forward declarations, for "friend" declarations
	namespace encap
	{
		class die;
		class dieset;
		struct loclist;
	}
}

#include "private/libdwarf.hpp"

namespace dwarf
{
	namespace core
	{
		using std::unique_ptr;
		using std::pair;
		using std::make_pair;
		using std::string;
		using std::map;
		using std::deque;
		using boost::optional;
		using boost::intrusive_ptr;
		using std::dynamic_pointer_cast;
		
		using dwarf::spec::opt;
		using namespace dwarf::lib;
		
#ifndef NO_TLS
		extern __thread Dwarf_Error current_dwarf_error;
#else
#warning "No TLS, so DWARF error reporting is not thread-safe."
		extern Dwarf_Error current_dwarf_error;
#endif
		
		// FIXME: temporary compatibility hack, please remove.... 
		typedef ::dwarf::spec::abstract_def spec;
		using dwarf::spec::DEFAULT_DWARF_SPEC; // FIXME: ... or get rid of spec:: namespace?
		
		// forward declarations
		struct root_die;
		struct iterator_base;
		
		struct dwarf3_factory_t;

		struct basic_die;
		struct compile_unit_die;
		struct program_element_die;

		/* This is a small interface designed to be implementable over both 
		 * libdwarf Dwarf_Die handles and whatever other representation we 
		 * choose. */
		struct abstract_die
		{
			// basically the "libdwarf methods" on core::Die...
			// ... but note that we can change the interface a little, e.g. see get_name()
			// ... to make it more generic but perhaps a little less efficient
			
			virtual Dwarf_Off get_offset() const = 0;
			virtual Dwarf_Half get_tag() const = 0;
			virtual opt<string> get_name() const = 0;
			// we can't do this because 
			// - it fixes string deletion behaviour to libdwarf-style, 
			// - it creates a circular dependency with the contents of libdwarf-handles.hpp
			//virtual unique_ptr<const char, string_deleter> get_raw_name() const = 0;
			virtual Dwarf_Off get_enclosing_cu_offset() const = 0;
			virtual bool has_attr(Dwarf_Half attr) const = 0;
			inline bool has_attribute(Dwarf_Half attr) const { return has_attr(attr); }
			virtual encap::attribute_map copy_attrs(opt<root_die&> opt_r/*, spec& s = DEFAULT_DWARF_SPEC*/) const = 0;
			/* HMM. Can we make this non-virtual? 
			 * Just move the code from Die (and get rid of that method)? */
			virtual spec& get_spec(root_die& r) const = 0;
			string summary() const;
		};
			
#include "private/libdwarf-handles.hpp"
		
		// now we can define factory
		struct factory
		{
			/* This mess is caused by spec bootstrapping. We have to construct 
			 * the CU payload to read its spec info, so this doesn't work in
			 * the case of CUs. All factories behave the same for CUs, 
			 * calling the make_cu_payload method. */
		protected:
			virtual basic_die *make_non_cu_payload(Die::handle_type&& it, root_die& r) = 0;
			compile_unit_die *make_cu_payload(Die::handle_type&& it, root_die& r);
		public:
			inline basic_die *make_payload(Die::handle_type&& it, root_die& r);
			static inline factory& for_spec(dwarf::spec::abstract_def& def);
			virtual basic_die *dummy_for_tag(Dwarf_Half tag) = 0;
		};

		
		class basic_die : public virtual abstract_die
		{
			friend struct iterator_base;
			friend class root_die;
		protected:
			// we need to embed a refcount
			unsigned refcount;
			
			// we need this, if we're libdwarf-backed; if not, it's null
			Die d;
			// we need this because it's a property of the CU, which we can't navigate to
			spec& s;

			/* No other fields! This class is here to act as the base for
			 * state-holding subclasses that are optimised for particular
			 * purposes, e.g. fast local/parameter location.  */
			// remember! payload = handle + shared count + extra state
			
			/* We define an overridable *interface* for attribute access. */
#define optional_root_arg_decl \
  opt<root_die&> opt_r 
#define optional_root_arg \
  optional_root_arg_decl = opt<root_die&>()
			virtual bool has_attr(Dwarf_Half attr) const 
			{ assert(d.handle); return d.has_attr_here(attr); }
			virtual encap::attribute_map all_attrs(optional_root_arg) const;
			virtual encap::attribute_value attr(Dwarf_Half a, optional_root_arg) const;
			root_die& get_root(opt<root_die&> opt_r) const // NOT defaulted!
			{ 
				return opt_r 
					? *opt_r 
					: (assert(d.handle), d.get_constructing_root()); 
			} // FIXME: replace with per-CU "default root" approach
			// NOTE: all functions passed an opt<root_die&> arg
			// which *delegate* to another function passing an opt<root_die&> arg
			// should... do what?
			// 1. pass the root they got from get_root()?
			// 2. pass the opt<root_die&> arg? 
			// 3. it doesn't matter?
			// 4. (DEFINITELY DON'T let it get defaulted)
			// 
			// regarding (3), it can only matter if the root *we* get, by doing get_root(), 
			// is not the same as the root the callee would get if it did get_root()
			// on the same argument. Can this happen? Only if there's an override of get_root()
			// in the target, *and* [this will only be different]
			// if the target is a different object (that chooses its default root differently).
			// Nevertheless, this could happen. 
			// Do we want to force the same root (good for consistency)
			// or respect the other object's different default (good for... hmm, not sure).
			// For now I will go with the former, i.e. "same root", meaning if we get_root() 
			// and therefore have our own r, we should delegate with that one. 
			
			// protected constructor constructing dummy instances only
			inline basic_die(spec& s): d(nullptr, 0), s(s) {}
			// protected constructor that is never actually used, but 
			// required to avoid special-casing in macros -- see begin_class() macro
			inline basic_die() : d(nullptr, nullptr), s(::dwarf::spec::DEFAULT_DWARF_SPEC) 
			{ assert(false); }
			friend struct dwarf3_factory_t;
		public:
			inline basic_die(spec& s, Die&& h)
			 : d(std::move(h)), s(s) {}
			
			friend std::ostream& operator<<(std::ostream& s, const basic_die& d);
			friend void intrusive_ptr_add_ref(basic_die *p);
			friend void intrusive_ptr_release(basic_die *p);
			
			virtual ~basic_die() {}
			
			/* implement the abstract_die interface 
			 * -- note that has_attr is defined above */
			inline Dwarf_Off get_offset() const { assert(d.handle); return d.offset_here(); }
			inline Dwarf_Half get_tag() const { assert(d.handle); return d.tag_here(); }
			inline opt<string> get_name() const 
			{ assert(d.handle); return opt<string>(string(d.name_here().get())); }
			inline unique_ptr<const char, string_deleter> get_raw_name() const
			{ assert(d.handle); return d.name_here(); }
			inline Dwarf_Off get_enclosing_cu_offset() const 
			{ assert(d.handle); return d.enclosing_cu_offset_here(); }
			//inline bool has_attr(Dwarf_Half attr) const 
			//{ assert(d.handle); return d.has_attr_here(attr); }
			inline encap::attribute_map copy_attrs(opt<root_die&> opt_r) const
			{ return encap::attribute_map(core::AttributeList(d), d, 
			  opt_r ? *opt_r : d.get_constructing_root(), s); }
			inline spec& get_spec(root_die& r) const 
			{ assert(d.handle); return d.spec_here(r); }
		};	
		std::ostream& operator<<(std::ostream& s, const basic_die& d);
		inline void intrusive_ptr_add_ref(basic_die *p)
		{
			++(p->refcount);
		}
		inline void intrusive_ptr_release(basic_die *p)
		{
			--(p->refcount);
			if (p->refcount == 0) delete p;
		}
		
		template <typename DerefAs /* = basic_die*/> struct iterator_df; // see attr.hpp
		template <typename DerefAs = basic_die> struct iterator_bf;
		template <typename DerefAs = basic_die> struct iterator_sibs;
		
		//template <typename Pred, typename DerefAs = basic_die> 
		//using iterator_sibs_where
		// = boost::filter_iterator< Pred, iterator_sibs<DerefAs> >;
		
		// FIXME: this is not libdwarf-agnostic! 
		// ** Could we use it for encap too, with a null Debug?
		// ** Can we abstract out a core base class
		// --- yes, where "Debug" just means "root resource", and encap doesn't have one
		// --- we are still leaking libdwarf design through our "abstract" interface
		// ------ can we do anything about this? 
		// --- what methods do handles provide? 
		struct root_die
		{
			/* Everything that calls a libdwarf constructor-style (resource-allocating)
			 * function needs to be our friend, so that it can get our raw
			 * Dwarf_Debug ptr. */
			friend struct iterator_base;
			friend struct Die;
			friend struct Attribute; // so it can get the raw Dwarf_Debug for its deleter
			friend struct AttributeList;
			friend struct Line;
			friend struct LineList;
			friend struct Global;
			friend struct GlobalList;
			friend struct Arange;
			friend struct ArangeList;
		public: // was protected -- consider changing back
			typedef intrusive_ptr<basic_die> ptr_type;

			map<Dwarf_Off, Dwarf_Off> parent_of;
			map<Dwarf_Off, ptr_type > sticky_dies; // compile_unit_die is always sticky
			Debug dbg;
			Dwarf_Off current_cu_offset; // 0 means none

			virtual ptr_type make_payload(const iterator_base& it)/* = 0*/;
			virtual bool is_sticky(const abstract_die& d) /* = 0*/;
			
		public:
			root_die(int fd) : dbg(fd), current_cu_offset(0UL) {}
			// we don't provide this constructor because sharing the CU state is a bad idea
			//root_die(lib::file& f) : dbg(f.dbg), current_cu_offset
		
			/* To make these policy-agnostic, we go with each policy being
			 * a separate iterator class.  The old policy-is-separate design
			 * has the problems that we don't know where to store copied 
			 * policies,  and that inlining is less likely to happen. */
			template <typename Iter = iterator_df<> >
			inline Iter begin(); 
			template <typename Iter = iterator_df<> >
			inline Iter end();
			template <typename Iter = iterator_df<> >
			inline pair<Iter, Iter> sequence();
			
			/* FIXME: instead of templating, maybe just return
			 * iterator_bases, and if you want a different iterator, use
			 * my_iter i = r.begin(); 
			 * ?
			 * (Just make sure my_iter has a move constructor!) */
			
			/* The implicit-conversion approach doesn't work so well for pairs, 
			 * because */
			inline pair<iterator_sibs<compile_unit_die>, iterator_sibs<compile_unit_die> >
			children_here();

			// const versions... nothing interesting here
			template <typename Iter = iterator_df<> >
			Iter begin() const { return const_cast<root_die*>(this)->begin<Iter>(); } 
			template <typename Iter = iterator_df<> >
			Iter end() const { return const_cast<root_die*>(this)->end<Iter>(); } 
			template <typename Iter = iterator_df<> >
			pair<Iter, Iter> sequence() const 
			{ return const_cast<root_die*>(this)->sequence<Iter>(); }
			template <typename Iter = iterator_df<compile_unit_die> >
			inline Iter enclosing_cu(const iterator_base& it);
			
			/* This is the expensive version. */
			template <typename Iter = iterator_df<> >
			Iter find(Dwarf_Off off);
			/* This is the cheap version -- must give a valid offset. */
			template <typename Iter = iterator_df<> >
			Iter pos(Dwarf_Off off, unsigned depth, 
				optional<Dwarf_Off> parent_off = optional<Dwarf_Off>() );
			/* This is a synonym for "pos()". */
			template <typename Iter = iterator_df<> >
			Iter at(Dwarf_Off off, unsigned depth, 
				optional<Dwarf_Off> parent_off = optional<Dwarf_Off>() )
			{ return pos<Iter>(off, depth, parent_off); }
			/* Convenience for getting CUs */
			template <typename Iter = iterator_df<compile_unit_die> >
			Iter cu_pos(Dwarf_Off off)
			{ return pos<Iter>(off, 1, optional<Dwarf_Off>()); }
			
			
			::Elf *get_elf(); // hmm: lib-only?
			Debug& get_dbg() { return dbg; }

			// iterator navigation primitives
			// note: want to avoid virtual dispatch on these
			bool move_to_parent(iterator_base& it);
			bool move_to_first_child(iterator_base& it);
			bool move_to_next_sibling(iterator_base& it);
			iterator_base parent(const iterator_base& it);
			iterator_base first_child(const iterator_base& it);
			iterator_base next_sibling(const iterator_base& it);
			// NOTE: we *don't* put named_child and move_to_named_child here, because
			// we want to allow exploitation of in-payload data, which might support
			// faster-than-linear search. So to do name lookups, we want to
			// call a method on the *iterator* (which knows whether it has payload).
			// It's okay to implement resolve() et al here, because they will call
			// into the iterator method.
			// BUT we do put a special find_named_child method, emphasising the linear
			// search (slow). This the fallback implementation used by the iterator.
			iterator_base find_named_child(const iterator_base& start, const string& name);
			
			// libdwarf has this weird stateful CU API
			optional<Dwarf_Off> first_cu_offset;
			optional<Dwarf_Unsigned> last_seen_cu_header_length;
			optional<Dwarf_Half> last_seen_version_stamp;
			optional<Dwarf_Unsigned> last_seen_abbrev_offset;
			optional<Dwarf_Half> last_seen_address_size;
			optional<Dwarf_Half> last_seen_offset_size;
			optional<Dwarf_Half> last_seen_extension_size;
			optional<Dwarf_Unsigned> last_seen_next_cu_header;
			bool advance_cu_context();
			bool clear_cu_context();
		protected:
			bool set_subsequent_cu_context(Dwarf_Off off); // helper
		public:
			bool set_cu_context(Dwarf_Off off);
			
			friend struct compile_unit_die; // redundant because we're struct, but future hint

			// print the whole lot
			friend std::ostream& operator<<(std::ostream& s, const root_die& d);

			/* Name resolution functions */
			template <typename Iter>
			inline iterator_base 
			resolve(const iterator_base& start, Iter path_pos, Iter path_end);
			inline iterator_base 
			resolve(const iterator_base& start, const std::string& name);

			template <typename Iter>
			inline iterator_base 
			scoped_resolve(const iterator_base& start, Iter path_pos, Iter path_end);
			inline iterator_base 
			scoped_resolve(const iterator_base& start, const string& name);
			
			template <typename Iter>
			inline void
			scoped_resolve_all(const iterator_base& start, Iter path_pos, Iter path_end, 
				std::vector<iterator_base >& results, int max = 0);
		};	
		std::ostream& operator<<(std::ostream& s, const root_die& d);
			
		/* Integrating with ADT: now we have two kinds of iterators.
		 * Core iterators, defined here, are fast, and when dereferenced
		 * yield references to reference-counted instances. 
		 * ADT iterators are slow, and yield shared_ptrs to instances. 
		 * Q. When is it okay to save these pointers? 
		 * A. Only when they are sticky! This is true in both ADT and
		 *    core cases. If we save a ptr/ref to a non-sticky DIE, 
		 *    in the core case it might go dangly, and in the ADT case
		 *    it might become disconnected (i.e. next time we get the 
		 *    same DIE, we will get a different instance, and if we make
		 *    changes, they needn't be reflected).
		 * To integrate these, is it as simple as
		 * - s/shared_ptr/intrusive_ptr/ in ADT;
		 * - include a refcount in every basic_die (basic_die_core?);
		 * - redefine abstract_dieset::iterator to be like core, but
		 *   returning the intrusive_ptr (not raw ref) on dereference? 
		 * Ideally I would separate out the namespaces so that
		 * - lib contains only libdwarf definitions
		 * - spec contains only spec-related things
		 * - "core" to contain the stuff in here
		 *   ... even though it is libdwarf-specific? HMM. 
		 *   ... perhaps reflect commonality by core::root_die, lib::root_die etc.?
		 *   ... here lib::root_die is : public core::root_die, but not polymorphic.
		 * - "adt" to contain spec:: ADT stuff
		 * - encap can stay as it is
		 * - lib ADT stuff should be migrated to core? 
		 * - encap ADT stuff should become an always-sticky variant?
		 * ... This involves unifying dieset/file_toplevel_die with root_die.
		 *  */
		struct iterator_base : private virtual abstract_die
		{
			/* Everything that calls a libdwarf constructor-style function
			 * needs to be a friend, so that it can get_raw_handle() to supply
			 * the argument to libdwarf.  */
			friend struct Die;
			friend struct Attribute;
			friend struct AttributeList;
			friend struct Arange;
			friend struct ArangeList;
			friend struct Line;
			friend struct LineList;
			friend struct Global;
			friend struct GlobalList;
			
			friend class root_die;
		private:
			/* This stuff is the Rep that can be abstracted out. */
			// union
			// {
				/* These guys are mutable because they need to be modifiable 
				 * even by, e.g., the copy constructor modifying its argument.
				 * The abstract value of the iterator isn't changed in such
				 * operations, but its representation must be. */
				mutable Die cur_handle; // to copy this, have to upgrade it
				mutable root_die::ptr_type cur_payload; // payload = handle + shared count + extra state
			// };
			mutable enum { HANDLE_ONLY, WITH_PAYLOAD } state;
			/* ^-- this is the absolutely key design point that makes this code fast. 
			 * An iterator can either be a libdwarf handle, or a pointer to some
			 * refcounted state (including such a handle, and maybe other cached stuff). */
			
			// normalization functions
			// this one is okay for public use,
			// *as long as* clients know that the handle might be null!
		public:
			abstract_die& get_handle() const
			{
				if (!is_real_die_position()) 
				{ assert(!cur_handle.handle && !cur_payload); return cur_handle; }
				switch (state)
				{
					case HANDLE_ONLY: return cur_handle;
					case WITH_PAYLOAD: return cur_payload->d;
					default: assert(false);
				}
			}
		private:
			//Die::raw_handle_type get_raw_handle() const
			//{
			//	return get_handle().raw_handle();
			//}
			// converse is iterator_base(handle) constructor (see below)
			
			/* This is more general-purpose stuff. */
			unsigned short m_depth; // HMM -- good value? does no harm space-wise atm
			root_die *p_root; // HMM -- necessary? alternative is: caller passes root& to some calls
			
		public:
			// we like to be default-constructible, BUT 
			// we are in an unusable state after this constructor
			// -- the same state as end()!
			iterator_base()
			 : cur_handle(Die(nullptr, nullptr)), state(HANDLE_ONLY), m_depth(0), p_root(nullptr) {}
			
			static const iterator_base END; // sentinel definition
			
			/* root position is encoded by null handle, null payload
			 * and non-null root pointer.
			 * cf. end position, which has null root pointer. */
			
			bool is_root_position() const 
			{ return p_root && !cur_handle.handle && !cur_payload; }  
			bool is_end_position() const 
			{ return !p_root && !cur_handle.handle && !cur_payload; }
			bool is_real_die_position() const 
			{ return !is_root_position() && !is_end_position(); } 
			
			// this constructor sets us up at begin(), i.e. the root DIE position
			explicit iterator_base(root_die& r)
			 : cur_handle(nullptr, nullptr), state(HANDLE_ONLY), m_depth(0), p_root(&r) {}
			
			// this constructor sets us up using a handle -- 
			// this does the exploitation of the sticky set
			iterator_base(Die&& d, unsigned depth, root_die& r)
			 : cur_handle(Die(nullptr, nullptr)) // will be replaced in function body...
			{
				// get the offset of the handle we've been passed
				Dwarf_Off off = d.get_offset(); 
				// is it an existing sticky DIE?
				auto found = r.sticky_dies.find(off);
				if (found != r.sticky_dies.end())
				{
					// sticky and exists
					cur_handle = Die(nullptr, nullptr);
					state = WITH_PAYLOAD;
					cur_payload = found->second;
					assert(cur_payload);
					//m_depth = found->second->get_depth(); assert(depth == m_depth);
					//p_root = &found->second->get_root();
					assert(r.is_sticky(static_cast<const abstract_die&>(d)));
				}
				else if (r.is_sticky(static_cast<const abstract_die&>(d)))
				{
					// should be sticky, but does not exist yet -- use the factory
					cur_handle = Die(nullptr, nullptr);
					state = WITH_PAYLOAD;
					cur_payload = factory::for_spec(d.spec_here(r)).make_payload(std::move(d.handle), r);
					assert(cur_payload);
					r.sticky_dies[off] = cur_payload;
				}
				else
				{
					// not sticky
					cur_handle = std::move(d.handle);
					state = HANDLE_ONLY;
				}
				m_depth = depth; // now shared by both cases
				p_root = &r;
			}
			
			
			/* GAH. Note that our iterator_base methods are impl'd like
			
					if (!is_real_die_position()) return false;
					return get_handle().has_attr_here(attr);
	
			 * 	SO... YES. Die should implement abstract_die, and get_handle()
			 * just does the switch and returns an abstract_die&
			 * either from payload or directly from the local linear handle. 
			 */
			
		public:
			// this constructor sets us up using a payload ptr
			// NO! can't go from payload to iterator, only other way
			//explicit iterator_base(root_die::ptr_type p)
			// : cur_handle(nullptr), state(WITH_PAYLOAD), 
			//   m_depth(p->get_depth()), p_root(&p->get_root()) {}
		
			// iterators must be copyable
			iterator_base(const iterator_base& arg)
			 : cur_handle(nullptr, nullptr), 
			   cur_payload(arg.is_real_die_position() ?
			   	  arg.get_root().make_payload(arg) 
				: nullptr),
			   state(arg.is_real_die_position() ? WITH_PAYLOAD : HANDLE_ONLY), 
			   m_depth(arg.depth()), 
			   p_root(arg.is_end_position() ? nullptr : &arg.get_root())
			{
				/* Now we're a copy, with payload. AND note that make_payload has modified arg 
				 * so that it is with payload too. */
				
				/* Sticky set: if we were passed a handle, and have therefore turned 
				 * it into a with_payload DIE, it should not have been sticky. That's
				 * because we don't create handle DIEs in the sticky case. To make sure,
				 * we assert this in make_payload. */

#ifdef DWARFPP_LINEAR_ITERATORS
				assert(!arg.is_real_die_position()); // linear mode: only copy non-real DIEs
#endif
				// FIXME: make sure root_die does the sticky set
			}
			
			// ... but we prefer to move them
			iterator_base(iterator_base&& arg)
			 : cur_handle(std::move(arg.cur_handle)), 
			   cur_payload(arg.cur_payload),
			   state(arg.state), 
			   m_depth(arg.depth()), 
			   p_root(arg.is_end_position() ? nullptr : &arg.get_root())
			{}
			
// 			// we can also get constructed directly in the WITH_PAYLOAD state, 
// 			// from a basic_die and a root 
// 			iterator_base(const basic_die& d, root_die& r)
// 			 : /* default-construct cur_handle */
// 			   cur_payload(root_die::ptr_type(&d)),
// 			   state(WITH_PAYLOAD), 
// 			   m_depth(d.m_depth), 
// 			   p_root(&r)
// 			{
// 			
// 			}
			/* NO! We don't support the above constructor, because we'd have to 
			 * get m_depth by searching. We go through root_die::find() instead. */ 
			
			// copy assignment			
			iterator_base& operator=(const iterator_base& arg) // does the upgrade...
			{ 
#ifdef DWARFPP_LINEAR_ITERATORS
				assert(false); // ... which we initially want to avoid (zero-copy)
#endif
				// we're a copy, so we can't have a handle
				this->cur_handle = std::move(Die(nullptr, nullptr));
				if (arg.is_real_die_position())
				{
					this->cur_payload = arg.get_root().make_payload(arg);
					this->state = WITH_PAYLOAD;
					this->m_depth = arg.depth();
					this->p_root = &arg.get_root();
				}
				else if (arg.is_end_position())
				{
					// NOTE: must put us in the same state as the default constructor
					this->cur_payload = nullptr;
					this->state = HANDLE_ONLY;
					this->m_depth = 0;
					this->p_root = nullptr;
				}
				else if (arg.is_root_position())
				{
					this->cur_payload = nullptr;
					this->state = HANDLE_ONLY; // the root DIE can get away with this (?)
					this->m_depth = 0;
					this->p_root = &arg.get_root();
				} else assert(false);

				return *this;
			}
			
			// move assignment
			iterator_base& operator=(iterator_base&& arg) = default;
			
			/* BUT note: these constructors are not enough, because unless we want
			 * users to construct raw handles, the user has no nice way of constructing
			 * a new iterator that is a sibling/child of an existing one. Note that
			 * this doesn't imply copying... we might want to create a new handle. */
		
			// convenience
			root_die& root() { assert(p_root); return *p_root; }
			root_die& root() const { assert(p_root); return *p_root; }
			root_die& get_root() { assert(p_root); return *p_root; }
			root_die& get_root() const { assert(p_root); return *p_root; }
		
			Dwarf_Off offset_here() const; 
			Dwarf_Half tag_here() const;
			
			opt<string> 
			name_here() const;
			
			inline spec& spec_here() const;
			
		private:
			/* implement the abstract_die interface
			 *  -- FIXME: this goes on Die instead? 
			 *  -- FIXME: why don't we inherit abstract_die? 
			 *  -- currently we have it on both Die and here. */
			inline Dwarf_Off get_offset() const { return offset_here(); }
			inline Dwarf_Half get_tag() const { return tag_here(); }
			// helper for raw names -> std::string names
			inline unique_ptr<const char, string_deleter> get_raw_name() const 
			{ return dynamic_cast<Die&>(get_handle()).name_here(); } 
			inline opt<string> get_name() const 
			{ return /*opt<string>(string(get_raw_name().get())); */ get_handle().get_name(); }
			inline Dwarf_Off get_enclosing_cu_offset() const 
			{ return enclosing_cu_offset_here(); }
			inline bool has_attr(Dwarf_Half attr) const { return has_attr_here(attr); }
			inline encap::attribute_map copy_attrs(opt<root_die&> opt_r) const
			{
				if (is_root_position()) return encap::attribute_map();
				
				if (state == HANDLE_ONLY)
				{
					return encap::attribute_map(
						AttributeList(dynamic_cast<Die&>(get_handle())),
						dynamic_cast<Die&>(get_handle()), 
						opt_r ? *opt_r : dynamic_cast<Die&>(get_handle()).get_constructing_root()
					);
				} 
				else 
				{
					assert(state == WITH_PAYLOAD);
					return cur_payload->all_attrs(opt_r);
				}
			}
			inline spec& get_spec(root_die& r) const { return spec_here(); }
			
		public:
			// getting all attributes -- exposed how? 
			// AttributeList is not very useful because each Attribute has no methods
			// Do we want to forward all (non-resource-allocating) "method"-like
			// libdwarf functions onto the raw handle type?
			// No.
			// But then we have to define an AttributeIterator?
			// + same for any other stuff (Line? Fde? Cie? etc.) 
			// that has method-like calls in the API.
			// How many of these are there?
			// Dwarf_Type is like this.
			// Probably lots of others too.
			// We should really port the methods over from lib::{die,attribute,...}.

			// COMMENT these out for now until I figure out how to expose attributes
// 			Attribute::handle_type attribute_here(Dwarf_Half attr)
// 			{
// 				/* HMM. This doesn't work on payloads. We can't assume that 
// 				 * libdwarf attributes are available. */
// 				return Attribute::try_construct(get_handle(), attr);
// 			}
// 			Attribute::handle_type attr_here(Dwarf_Half attr) 
// 			{ return attribute_here(attr); }
			
			bool has_attr_here(Dwarf_Half attr) const;
			bool has_attribute_here(Dwarf_Half attr) const { return has_attr_here(attr); }
			
			/* FIXME: why not return an AttributeList rvalue? 
			 * This would avoid explicit constructor in iterator_base's operator<< (in lib.cpp).
			 * Possible answer: want a "maybe"/"optional" return value? BUT
			 * hmm, AttributeList should always be constructable, just empty if no attrs. */
			/* FIXME: we don't yet have a "generic" implementation of an attribute collection, 
			 * cf. abstract_die w.r.t. Die. So for now, these methods only work on libdwarf
			 * (Die) dies, whereas they should also work on encap-like dies too.  */
			AttributeList::handle_type attributes_here()
			{ return AttributeList::try_construct(dynamic_cast<Die&>(get_handle())); }
			AttributeList::handle_type attrs_here() { return attributes_here(); }
			AttributeList::handle_type attributes_here() const 
			{ return AttributeList::try_construct(dynamic_cast<Die&>(get_handle())); }
			AttributeList::handle_type attrs_here() const { return attributes_here(); }
			
			// want an iterators-style interface?
			// or an associative-style operator[] interface?
			// or both?
			// make payload include a deep copy of the attrs state? 
			// HMM, yes, this feels correct. 
			// Clients that want to scan attributes in a lightweight libdwarf way
			// (i.e. not benefiting from caching/ custom representations / 
			// utility methods implemented using payload state) can use 
			// the AttributeList interface.
					
			// some fast topological queries
			Dwarf_Off enclosing_cu_offset_here() const
			{ return get_handle().get_enclosing_cu_offset(); }
			unsigned depth() const { return m_depth; }
			unsigned get_depth() const { return m_depth; }
			
			// access to children, siblings, parent, ancestors
			// -- these wrap the various Die constructors
			iterator_base nearest_enclosing(Dwarf_Half tag) const;
			iterator_base parent() const;
			iterator_base first_child() const;
			iterator_base next_sibling() const;			
			iterator_base named_child(const string& name) const;
			// + resolve? no, I have put resolve stuff on the root_die
			inline iterator_df<compile_unit_die> enclosing_cu() const;
			
			// children
			// so how do we iterate over "children satisfying predicate, derefAs'd X"? 
			//pair< 
			//    filter_iterator< predicate_t, iterator_sibs< X > >,
			//    filter_iterator< predicate_t, iterator_sibs< X > >
			template <typename Payload>
			struct is_a_t
			{
				inline bool operator()(const iterator_base& it) const;
			}; // defined below, once we have factory
			
			template <typename Payload>
			inline bool is_a() const
			{
				return is_a_t<Payload>()(*this);
			}
			
			// We want to partially specialize a function template, 
			// which we can't do. So pull out the core into a class
			// template which we call from the (non-specialised) function template.
			template <typename Pred, typename Iter>
			struct subseq_t
			{
				typedef srk31::selective_iterator<Pred, Iter> filtered_iterator;
				
				pair<filtered_iterator, filtered_iterator> 
				operator()(const pair<Iter, Iter>& in_seq)
				{ 
					return make_pair(
						filtered_iterator(in_seq.first, in_seq.second),
						filtered_iterator(in_seq.second, in_seq.second)
					);
				}

				pair<filtered_iterator, filtered_iterator> 
				operator()(pair<Iter, Iter>&& in_seq)
				{ 
					/* GAH. We can only move if .second == iterator_base::END, because 
					 * otherwise we have to duplicate the end iterator into both 
					 * filter iterators. */
					assert(in_seq.second == iterator_base::END);
					return make_pair(
						filtered_iterator(std::move(in_seq.first), iterator_base::END),
						filtered_iterator(std::move(in_seq.second), iterator_base::END)
					);
				}			
			};
			// specialization for is_a, adding downcast transformer
			template <typename Iter, typename Payload>
			struct subseq_t<Iter, is_a_t<Payload> >
			{
				typedef srk31::selective_iterator< is_a_t<Payload>, Iter> filtered_iterator;

				// transformer is just dynamic_cast, wrapped as a function
				typedef Payload& derived_ref;
				typedef basic_die& base_ref;
				typedef std::function<derived_ref(base_ref)> transformer;
				inline static derived_ref transf(base_ref arg) { 
					return dynamic_cast<Payload&>(arg);
				}

				typedef boost::transform_iterator<transformer, filtered_iterator >
					transformed_iterator;
				
				/* HMM. Actually we want to get the transform_iterator type, then 
				 * mix it in with the original iterator type. Can we use CRTP for this? */
				pair<transformed_iterator, transformed_iterator> 
				operator()(const pair<Iter, Iter>& in_seq)
				{
					auto filtered_first = filtered_iterator(in_seq.first, in_seq.second);
					auto filtered_second = filtered_iterator(in_seq.second, in_seq.second);
					
					return make_pair(
						transformed_iterator(
							filtered_first,
							transf
						),
						transformed_iterator(
							filtered_second,
							transf
						)
					);
				}
				
				pair<transformed_iterator, transformed_iterator> 
				operator()(pair<Iter, Iter>&& in_seq)
				{
					/* GAH. We can only move if .second == iterator_base::END, because 
					 * otherwise we have to duplicate the end sentinel into both 
					 * filter iterators. */
					if (in_seq.second == iterator_base::END)
					{
						// NOTE: this std::move is all for nothing at the moment because 
						// transform_iterator doesn't implement move constuctor/assignment.
						
						auto filtered_first = filtered_iterator(std::move(in_seq.first), iterator_base::END);
						auto filtered_second = filtered_iterator(std::move(in_seq.second), iterator_base::END);
											
						return make_pair(
							std::move(transformed_iterator(
								std::move(filtered_first),
								transf
							)),
							std::move(transformed_iterator(
								std::move(filtered_second),
								transf
							))
						);
					} else { auto tmp = in_seq; return operator()(tmp); } // copying version
				}
			};

			/* With the above, we should be able to write
			 * auto subps = iterator_base::subseq< is_a_t<subprogram_die> >(i_cu.children_here());
			 * ... but let's go one better:
			 * auto subps = i_cu.children_here().subseq_of<subprogram_die>();
			 * ... by defining a special "subsequent" wrapper of iterator-pairs.
			 */
					
			template <typename Iter> 
			struct sequence : public pair<Iter, Iter>
			{
				typedef pair<Iter, Iter> base; 
				sequence(pair<Iter, Iter>&& arg) : base(std::move(arg)) {}
				
				template <typename Pred>
				sequence<
					srk31::selective_iterator<Pred, Iter>
				> subseq_with(Pred pred = Pred()) 
				{ return iterator_base::subseq_t<Pred, Iter>().operator()(std::move(*this)); }
				
				template <typename D>
				sequence<
					typename subseq_t<Iter, is_a_t<D> >::transformed_iterator
				> subseq_of() 
				{ 
					return iterator_base::subseq_t< 
						Iter, 
						iterator_base::is_a_t<D> 
					>().operator()(std::move(*this)); 
				}
			};

			inline sequence<iterator_sibs<> >
			children_here();
			inline sequence<iterator_sibs<> >
			children_here() const;
			// synonyms
			inline sequence<iterator_sibs<> > children();
			inline sequence<iterator_sibs<> > children() const;
			
			// we're just the base, not the iterator proper, 
			// so we don't have increment(), decrement()
			
			bool operator==(const iterator_base& arg) const
			{
				if (!p_root && !arg.p_root) return true; // END
				// now we're either root or "real". Handle the case where we're root. 
				if (!cur_handle.handle.get() 
					&& !arg.cur_handle.handle.get()) return p_root == arg.p_root;
				// now at least one of us is "real"...
				//if (state == HANDLE_ONLY)
				//{
				//	// ... and we can't be equal because we should be unique // NO, WRONG; see below
				//	else return false;
				//}
				//else
				//{
				// NOTE: the above logic is wrong because we can ask libdwarf for a 
				// fresh handle at the same offset, and it will be distinct.
				
					return this->p_root == arg.p_root
						&& this->offset_here() == arg.offset_here();
				//}
			}
			bool operator!=(const iterator_base& arg) const	
			{ return !(*this == arg); }
			
			basic_die& dereference() const 
			{
				return *get_root().make_payload(*this);
			}
			
			friend std::ostream& operator<<(std::ostream& s, const iterator_base& it);
		}; 
		/* END class iterator_base */
	} namespace spec {
		template <>
		struct opt< core::iterator_base > : core::iterator_base
		{
			typedef core::iterator_base super;
			opt() : super() {}
			opt( none_t none_ ) : super(/*none_*/) {}
			opt ( const iterator_base& val ) : super(val) {}
			opt ( bool cond, const core::iterator_base& val ) : super(cond ? val : core::iterator_base::END) {}
#ifndef BOOST_OPTIONAL_NO_CONVERTING_COPY_CTOR
			template<class U>
			explicit opt ( opt<U> const& rhs )
			  :
			  super()
			{
			  if ( rhs.is_initialized() )
				*this = rhs.get(); //this->construct(rhs.get());
			}
#endif
#ifndef BOOST_OPTIONAL_NO_INPLACE_FACTORY_SUPPORT
			template<class Expr>
			explicit opt ( Expr const& expr ) : super(expr,boost::addressof(expr)) {}
#endif
			opt(const opt< core::iterator_base >& arg) : super((const super&)arg) {}
			opt< core::iterator_base >& operator=(const opt<core::iterator_base >& arg) 
			{ *((super *) this) = (const super&) arg; return *this; }
		};
	} namespace core {
		/* END specialization for opt<iterator_base<DerefAs> > */
		using ::dwarf::spec::opt;
		
		std::ostream& operator<<(std::ostream& s, const iterator_base& it);
		std::ostream& operator<<(std::ostream& s, const AttributeList& attrs);
		
		/* Time for some factory subclasses. */
		struct dwarf3_factory_t : public factory
		{
			basic_die *make_non_cu_payload(Die::handle_type&& it, root_die& r);
			basic_die *dummy_for_tag(Dwarf_Half tag);
		};
		extern dwarf3_factory_t dwarf3_factory;
		inline factory& factory::for_spec(dwarf::spec::abstract_def& def)
		{
			if (&def == &dwarf::spec::dwarf3_def::inst) return dwarf3_factory;
			assert(false); // FIXME support more specs
		}

		/* Now we can define that pesky template operator function. 
		 * The factory exposes a dummy method (NOT type-level though! it's 
		 * polymorphic!) that returns us a fake singleton of any instantiable  
		 * DIE type. */
		template <typename Payload>
		inline bool iterator_base::is_a_t<Payload>::operator()(const iterator_base& it) const
		{
			return dynamic_cast<Payload *>(
				factory::for_spec(it.spec_here()).dummy_for_tag(it.tag_here())
			) ? true : false;
		}
		/* Make sure we can construct any iterator from an iterator_base. 
		 * In the case of BFS it may be expensive. */
		template <typename DerefAs /* = basic_die */>
		struct iterator_df : public iterator_base,
		                     public boost::iterator_facade<
		                       iterator_df<DerefAs>
		                     , DerefAs
		                     , boost::forward_traversal_tag
		                     , DerefAs& //boost::use_default /* Reference */
		                     , Dwarf_Signed /* difference */
		                     >
		{
			typedef iterator_df<DerefAs> self;
			typedef DerefAs DerefType;
			friend class boost::iterator_core_access;
			
			iterator_base& base_reference()
			{ return static_cast<iterator_base&>(*this); }
			const iterator_base& base() const
			{ return static_cast<const iterator_base&>(*this); }
			
			iterator_df() : iterator_base() {}
			iterator_df(const iterator_base& arg)
			 : iterator_base(arg) {}// this COPIES so avoid
			iterator_df(iterator_base&& arg)
			 : iterator_base(arg) {}
			
			iterator_df& operator=(const iterator_base& arg) 
			{ this->base_reference() = arg; return *this; }
			iterator_df& operator=(iterator_base&& arg) 
			{ this->base_reference() = std::move(arg); return *this; }
			
			void increment()
			{
				if (get_root().move_to_first_child(base_reference())) return;
				do
				{
					if (get_root().move_to_next_sibling(base_reference())) return;
				} while (get_root().move_to_parent(base_reference()));

				// if we got here, there is nothing left in the tree...
				// ... so set us to the end sentinel
				base_reference() = base_reference().get_root().end/*<self>*/();
			}
			void decrement()
			{
				assert(false); // FIXME
			}
			bool equal(const self& arg) const { return this->base() == arg.base(); }
			
			DerefAs& dereference() const
			{ return dynamic_cast<DerefAs&>(this->iterator_base::dereference()); }
		};
		
		template <typename DerefAs /* = basic_die */>
		struct iterator_bf : public iterator_base,
		                     public boost::iterator_facade<
		                       iterator_bf<DerefAs>
		                     , DerefAs
		                     , boost::forward_traversal_tag
		                     , DerefAs& // boost::use_default /* Reference */
		                     , Dwarf_Signed /* difference */
		                     >
		{
			typedef iterator_bf<DerefAs> self;
			friend class boost::iterator_core_access;

			// extra state needed!
			deque< iterator_base > m_queue;
			
			iterator_base& base_reference()
			{ return static_cast<iterator_base&>(*this); }
			const iterator_base& base() const
			{ return static_cast<const iterator_base&>(*this); }
			
			iterator_bf() : iterator_base() {}
			iterator_bf(const iterator_base& arg)
			 : iterator_base(arg) {}// this COPIES so avoid
			iterator_bf(iterator_base&& arg)
			 : iterator_base(arg) {}
			iterator_bf(const iterator_bf<DerefAs>& arg)
			 : iterator_base(arg), m_queue(arg.m_queue) {}// this COPIES so avoid
			iterator_bf(iterator_bf<DerefAs>&& arg)
			 : iterator_base(arg), m_queue(std::move(arg.m_queue)) {}
			
			iterator_bf& operator=(const iterator_base& arg) 
			{ this->base_reference() = arg; this->m_queue.clear(); return *this; }
			iterator_bf& operator=(iterator_base&& arg) 
			{ this->base_reference() = std::move(arg); this->m_queue.clear(); return *this; }
			iterator_bf& operator=(const iterator_bf<DerefAs>& arg) 
			{ this->base_reference() = arg; this->m_queue = arg.m_queue; return *this; }
			iterator_bf& operator=(iterator_bf<DerefAs>&& arg) 
			{ this->base_reference() = std::move(arg); this->m_queue =std::move(arg.m_queue); return *this; }

			void increment()
			{
				/* Breadth-first traversal:
				 * - move to the next sibling if there is one, 
				 *   enqueueing first child (if there is one);
				 * - else take from the queue, if non empty
				 * - else fail (terminated)
				 */
				auto first_child = get_root().first_child(this->base_reference()); 
				//   ^-- might be END
				
				if (get_root().move_to_next_sibling(this->base_reference()))
				{
					// success, so enqueue the first child if there is one
					if (first_child != iterator_base::END) m_queue.push_back(first_child);
				}
				else if (m_queue.size() > 0)
				{
					this->base_reference() = m_queue.front(); m_queue.pop_front();
				}
				else
				{
					this->base_reference() = iterator_base::END;
				}
			}
			
			void increment_skipping_subtree()
			{
				/* This is the same as increment, except we are not interested in children 
				 * of the current node. */
				if (get_root().move_to_next_sibling(this->base_reference()))
				{
					// success -- don't enqueue children
					return;
				}
				else if (m_queue.size() > 0)
				{
					this->base_reference() = m_queue.front(); m_queue.pop_front();
				}
				else
				{
					this->base_reference() = iterator_base::END;
				}
			}
			
			void decrement()
			{
				assert(false); // FIXME
			}
			DerefAs& dereference() const
			{ return dynamic_cast<DerefAs&>(this->iterator_base::dereference()); }
		};
		
		template <typename DerefAs /* = basic_die*/>
		struct iterator_sibs : public iterator_base,
		                       public boost::iterator_facade<
		                       iterator_sibs<DerefAs> /* I (CRTP) */
		                     , DerefAs /* V */
		                     , boost::forward_traversal_tag
		                     , DerefAs& //boost::use_default /* Reference */
		                     , Dwarf_Signed /* difference */
		                     >
		{
			typedef iterator_sibs<DerefAs> self;
			friend class boost::iterator_core_access;
			
			iterator_base& base_reference()
			{ return static_cast<iterator_base&>(*this); }
			const iterator_base& base() const
			{ return static_cast<const iterator_base&>(*this); }
			
			iterator_sibs() : iterator_base()
			{}

			iterator_sibs(const iterator_base& arg)
			 : iterator_base(arg)
			{}

			iterator_sibs(iterator_base&& arg)
			 : iterator_base(std::move(arg))
			{}
			
			iterator_sibs& operator=(const iterator_base& arg) 
			{ this->base_reference() = arg; return *this; }
			iterator_sibs& operator=(iterator_base&& arg) 
			{ this->base_reference() = std::move(arg); return *this; }
			
			void increment()
			{
				if (this->base_reference().get_root().move_to_next_sibling(this->base_reference())) return;
				else { this->base_reference() = this->base_reference().get_root().end/*<self>*/(); return; }
			}
			
			void decrement()
			{
				assert(false);
			}
			
			bool equal(const self& arg) const { return this->base() == arg.base(); }
			DerefAs& dereference() const
			{ return dynamic_cast<DerefAs&>(this->iterator_base::dereference()); }
		};


/****************************************************************/
/* begin generated ADT includes                                 */
/****************************************************************/
#define forward_decl(t) class t ## _die;
#define declare_base(base) base ## _die
#define initialize_base(fragment) /*fragment ## _die(s, std::move(h))*/
#define constructor(fragment, ...) /* "..." is base inits */ \
		fragment ## _die(spec& s, Die&& h) : basic_die(s, std::move(h)) {}
/* #define declare_bases(first_base, ...) first_base , ##__VA_ARGS__ */
#define begin_class(fragment, base_inits, ...) \
	struct fragment ## _die : virtual __VA_ARGS__ { \
	friend struct dwarf3_factory_t; \
	private: /* dummy constructor used only to construct dummy instances */\
		fragment ## _die(spec& s) : basic_die(s, Die(nullptr, nullptr)) {} \
	public: /* main constructor */ \
		constructor(fragment, base_inits /* NOTE: base_inits is expanded via ',' into varargs list */) \
	protected: /* protected constructor that doesn't touch basic_die */ \
		fragment ## _die() {} \
	public: /* extra decls should be public */
#define base_initializations(...) __VA_ARGS__
#define end_class(fragment) \
	};

#define stored_type_string std::string
#define stored_type_flag bool
#define stored_type_unsigned Dwarf_Unsigned
#define stored_type_signed Dwarf_Signed
#define stored_type_offset Dwarf_Off
#define stored_type_half Dwarf_Half
#define stored_type_ref Dwarf_Off
#define stored_type_tag Dwarf_Half
#define stored_type_loclist dwarf::encap::loclist
#define stored_type_address dwarf::encap::attribute_value::address
#define stored_type_refiter iterator_df<basic_die>
#define stored_type_refiter_is_type iterator_df<type_die>
#define stored_type_rangelist dwarf::encap::rangelist

/* This is libdwarf-specific. This might be okay -- 
 * depends if we want a separate class hierarchy for the 
 * encap-style ones (like in encap::). Yes, we probably do.
 * BUT things like type_die should still be the supertype, right?
 * ARGH. 
 * I think we can use virtuality to deal with this in one go. 
 * Define a new encapsulated_die base class (off of basic_die) 
 * which redefines all the get_() stuff in one go. 
 * and then an encapsulated version of any type_die can use
 * virtual inheritance to wire its getters up to those versions. 
 * ARGH: no, we need another round of these macros to enumerate all the getters. 
 */
#define attr_optional(name, stored_t) \
	opt<stored_type_ ## stored_t> get_ ## name(optional_root_arg) const \
    { if (has_attr(DW_AT_ ## name)) return attr(DW_AT_ ## name, opt_r).get_ ## stored_t (); \
      else return opt<stored_type_ ## stored_t>(); }

#define super_attr_optional(name, stored_t) attr_optional(name, stored_t)

#define attr_mandatory(name, stored_t) \
	stored_type_ ## stored_t get_ ## name(optional_root_arg) const \
    { assert (d.has_attr_here(DW_AT_ ## name)); \
      return attr(DW_AT_ ## name, opt_r).get_ ## stored_t (); }

#define super_attr_mandatory(name, stored_t) attr_mandatory(name, stored_t)
#define child_tag(arg)
/* here we hack the to-be-included file s.t. it has s/refdie/refiter/ */
#define stored_type_refdie stored_type_refiter
#define stored_type_refdie_is_type stored_type_refiter_is_type
#define get_refdie get_refiter
#define get_refdie_is_type get_refiter_is_type

struct type_die; 
} namespace lib {
struct regs;
} namespace core {
	struct with_static_location_die : public virtual basic_die
	{
		struct sym_binding_t 
		{ 
			Dwarf_Off file_relative_start_addr; 
			Dwarf_Unsigned size;
		};
		virtual encap::loclist get_static_location(optional_root_arg) const;
		opt<Dwarf_Off> spans_addr(
			Dwarf_Addr file_relative_addr,
			root_die& r,
			sym_binding_t (*sym_resolve)(const std::string& sym, void *arg) = 0, 
			void *arg = 0) const;
		virtual boost::icl::interval_map<Dwarf_Addr, Dwarf_Unsigned> 
		file_relative_intervals(
			root_die& r, 
			sym_binding_t (*sym_resolve)(const std::string& sym, void *arg), 
			void *arg /* = 0 */) const;
	};

	struct with_type_describing_layout_die : public virtual basic_die
	{
		virtual opt<iterator_df<type_die> > get_type(optional_root_arg) const = 0;
	};

	struct with_dynamic_location_die : public virtual with_type_describing_layout_die
	{
		virtual opt<Dwarf_Off> spans_addr(
				Dwarf_Addr absolute_addr,
				Dwarf_Signed instantiating_instance_addr, 
				root_die& r, 
				Dwarf_Off dieset_relative_ip,
				dwarf::lib::regs *p_regs = 0) const = 0;
		/* We define two variants of the spans_addr logic: 
		 * one suitable for stack-based locations (fp/variable)
		 * and another for object-based locations (member/inheritance)
		 * and each derived class should pick one! */
	protected:
		opt<Dwarf_Off> spans_stack_addr(
				Dwarf_Addr absolute_addr,
				Dwarf_Signed instantiating_instance_addr,
				root_die& r,
				Dwarf_Off dieset_relative_ip,
				dwarf::lib::regs *p_regs = 0) const;
		opt<Dwarf_Off> spans_addr_in_object(
				Dwarf_Addr absolute_addr,
				Dwarf_Signed instantiating_instance_addr,
				root_die& r, 
				Dwarf_Off dieset_relative_ip,
				dwarf::lib::regs *p_regs = 0) const;
	public:
		virtual iterator_df<program_element_die> 
		get_instantiating_definition(optional_root_arg) const;

		virtual Dwarf_Addr calculate_addr(
			Dwarf_Addr instantiating_instance_location,
			root_die& r,
			Dwarf_Off dieset_relative_ip,
			dwarf::lib::regs *p_regs = 0) const = 0;

		/** This gets a location list describing the location of the thing, 
			assuming that the instantiating_instance_location has been pushed
			onto the operand stack. */
		virtual encap::loclist get_dynamic_location(optional_root_arg) const = 0;
	protected:
		/* ditto */
		virtual Dwarf_Addr calculate_addr_on_stack(
			Dwarf_Addr instantiating_instance_location,
			root_die& r,
			Dwarf_Off dieset_relative_ip,
			dwarf::lib::regs *p_regs = 0) const;
		virtual Dwarf_Addr calculate_addr_in_object(
			Dwarf_Addr instantiating_instance_location,
			root_die& r, 
			Dwarf_Off dieset_relative_ip,
			dwarf::lib::regs *p_regs = 0) const;
	public:

		/* virtual Dwarf_Addr calculate_addr(
			Dwarf_Signed frame_base_addr,
			Dwarf_Off dieset_relative_ip,
			dwarf::lib::regs *p_regs = 0) const;*/
	};

	struct with_named_children_die : public virtual basic_die
	{
		/* We most most of the resolution stuff into iterator_base, 
		 * but leave this here so that payload implementations can
		 * provide a faster-than-default (i.e. faster than linear search)
		 * way to look up named children (e.g. hash table). */
		virtual 
		inline iterator_base
		named_child(const std::string& name, optional_root_arg) const;
	};

/* program_element_die */
begin_class(program_element, base_initializations(basic), declare_base(basic))
		attr_optional(decl_file, unsigned)
		attr_optional(decl_line, unsigned)
		attr_optional(decl_column, unsigned)
		attr_optional(prototyped, flag)
		attr_optional(declaration, flag)
		attr_optional(external, flag)
		attr_optional(visibility, unsigned)
		attr_optional(artificial, flag)
end_class(program_element)
/* type_die */
begin_class(type, base_initializations(initialize_base(program_element)), declare_base(program_element))
		attr_optional(byte_size, unsigned)
		opt<Dwarf_Unsigned> calculate_byte_size(optional_root_arg) const;
		bool is_rep_compatible(iterator_df<type_die> arg, optional_root_arg) const;
		iterator_df<type_die> get_concrete_type(optional_root_arg) const;
		iterator_df<type_die> get_unqualified_type(optional_root_arg) const;
end_class(type)
/* type_chain_die */
begin_class(type_chain, base_initializations(initialize_base(type)), declare_base(type))
        attr_optional(type, refdie_is_type)
        opt<Dwarf_Unsigned> calculate_byte_size(optional_root_arg) const;
        iterator_df<type_die> get_concrete_type(optional_root_arg) const;
end_class(type_chain)
/* qualified_type_die */
begin_class(qualified_type, base_initializations(initialize_base(type_chain)), declare_base(type_chain))
        iterator_df<type_die> get_unqualified_type(optional_root_arg) const;
end_class(qualified_type)
/* with_data_members_die */
begin_class(with_data_members, base_initializations(initialize_base(type)), declare_base(type))
        child_tag(member)
		iterator_df<type_die> find_my_own_definition(optional_root_arg) const; // for turning declarations into defns
end_class(with_data_members)

#define has_stack_based_location \
	opt<Dwarf_Off> spans_addr( \
                    Dwarf_Addr aa, \
                    Dwarf_Signed fb, \
					root_die& r, \
                    Dwarf_Off dr_ip, \
                    dwarf::lib::regs *p_regs) const \
					{ return spans_stack_addr(aa, fb, r, dr_ip, p_regs); } \
	Dwarf_Addr calculate_addr( \
				Dwarf_Addr fb, \
				root_die& r, \
				Dwarf_Off dr_ip, \
				dwarf::lib::regs *p_regs = 0) const \
				{ return calculate_addr_on_stack(fb, r, dr_ip, p_regs); } \
	encap::loclist get_dynamic_location(optional_root_arg_decl) const;
#define has_object_based_location \
	opt<Dwarf_Off> spans_addr( \
                    Dwarf_Addr aa, \
                    Dwarf_Signed io, \
					root_die& r, \
                    Dwarf_Off dr_ip, \
                    dwarf::lib::regs *p_regs) const \
					{ return spans_addr_in_object(aa, io, r, dr_ip, p_regs); } \
	Dwarf_Addr calculate_addr( \
				Dwarf_Addr io, \
				root_die& r, \
				Dwarf_Off dr_ip, \
				dwarf::lib::regs *p_regs = 0) const \
				{ return calculate_addr_in_object(io, r, dr_ip, p_regs); } \
	encap::loclist get_dynamic_location(optional_root_arg_decl) const;
#define extra_decls_subprogram \
        opt< std::pair<Dwarf_Off, iterator_df<with_dynamic_location_die> > > \
        spans_addr_in_frame_locals_or_args( \
                    Dwarf_Addr absolute_addr, \
					root_die& r, \
                    Dwarf_Off dieset_relative_ip, \
                    Dwarf_Signed *out_frame_base, \
                    dwarf::lib::regs *p_regs = 0) const; \
        bool is_variadic(optional_root_arg) const; 
#define extra_decls_variable \
        bool has_static_storage(optional_root_arg) const; \
		has_stack_based_location
#define extra_decls_formal_parameter \
		has_stack_based_location
#define extra_decls_array_type \
		opt<Dwarf_Unsigned> element_count(optional_root_arg) const; \
        opt<Dwarf_Unsigned> calculate_byte_size(optional_root_arg) const; \
        bool is_rep_compatible(iterator_df<type_die> arg, optional_root_arg) const; \
		iterator_df<type_die> ultimate_element_type(optional_root_arg) const; \
		opt<Dwarf_Unsigned> ultimate_element_count(optional_root_arg) const; 
#define extra_decls_pointer_type \
		iterator_df<type_die> get_concrete_type(optional_root_arg) const; \
        opt<Dwarf_Unsigned> calculate_byte_size(optional_root_arg) const; \
        bool is_rep_compatible(iterator_df<type_die> arg, optional_root_arg) const;
#define extra_decls_reference_type \
		iterator_df<type_die> get_concrete_type(optional_root_arg) const; \
        opt<Dwarf_Unsigned> calculate_byte_size(optional_root_arg) const; \
        bool is_rep_compatible(iterator_df<type_die> arg, optional_root_arg) const;
#define extra_decls_base_type \
		bool is_rep_compatible(iterator_df<type_die> arg, optional_root_arg) const;
#define extra_decls_structure_type \
		opt<Dwarf_Unsigned> calculate_byte_size(optional_root_arg) const; \
		bool is_rep_compatible(iterator_df<type_die> arg, optional_root_arg) const; 
#define extra_decls_union_type \
		bool is_rep_compatible(iterator_df<type_die> arg, optional_root_arg) const; 
#define extra_decls_class_type \
		bool is_rep_compatible(iterator_df<type_die> arg, optional_root_arg) const; 
#define extra_decls_enumeration_type \
		bool is_rep_compatible(iterator_df<type_die> arg, optional_root_arg) const;
#define extra_decls_subroutine_type \
		bool is_rep_compatible(iterator_df<type_die> arg, optional_root_arg) const;
#define extra_decls_member \
		opt<Dwarf_Unsigned> byte_offset_in_enclosing_type(optional_root_arg) const; \
		has_object_based_location
#define extra_decls_inheritance \
		opt<Dwarf_Unsigned> byte_offset_in_enclosing_type(optional_root_arg) const; \
		has_object_based_location

#define extra_decls_compile_unit \
/* We define fields and getters for the per-CU source file info */ \
/* (which is *separate* from decl_file and decl_line attributes). */ \
public: \
inline std::string source_file_name(unsigned o) const; \
inline unsigned source_file_count() const; \
/* We define fields and getters for the per-CU info (NOT attributes) */ \
/* available from libdwarf. These will be filled in by root_die::make_payload(). */ \
protected: \
Dwarf_Unsigned cu_header_length; \
Dwarf_Half version_stamp; \
Dwarf_Unsigned abbrev_offset; \
Dwarf_Half address_size; \
Dwarf_Half offset_size; \
Dwarf_Half extension_size; \
Dwarf_Unsigned next_cu_header; \
public: \
Dwarf_Unsigned get_cu_header_length() const { return cu_header_length; } \
Dwarf_Half get_version_stamp() const { return version_stamp; } \
Dwarf_Unsigned get_abbrev_offset() const { return abbrev_offset; } \
Dwarf_Half get_address_size() const { return address_size; } \
Dwarf_Half get_offset_size() const { return offset_size; } \
Dwarf_Half get_extension_size() const { return extension_size; } \
Dwarf_Unsigned get_next_cu_header() const { return next_cu_header; } \
opt<Dwarf_Unsigned> implicit_array_base(optional_root_arg) const; \
iterator_df<type_die> implicit_enum_base_type(optional_root_arg) const; \
encap::rangelist normalize_rangelist(const encap::rangelist& rangelist) const; \
friend class iterator_base; \
friend class factory; 

#include "dwarf3-adt.h"

#undef get_refdie_is_type
#undef get_refdie
#undef stored_type_refdie_is_type
#undef stored_type_refdie

#undef extra_decls_subprogram
#undef extra_decls_compile_unit
#undef extra_decls_array_type
#undef extra_decls_variable
#undef extra_decls_structure_type
#undef extra_decls_union_type
#undef extra_decls_class_type
#undef extra_decls_enumeration_type
#undef extra_decls_subroutine_type
#undef extra_decls_member
#undef extra_decls_inheritance
#undef extra_decls_pointer_type
#undef extra_decls_reference_type
#undef extra_decls_base_type
#undef extra_decls_formal_parameter

#undef has_stack_based_location
#undef has_object_based_location

#undef forward_decl
#undef declare_base
#undef declare_bases
#undef base_fragment
#undef initialize_base
#undef constructor
#undef begin_class
#undef base_initializations
#undef declare_base
#undef end_class
#undef stored_type
#undef stored_type_string
#undef stored_type_flag
#undef stored_type_unsigned
#undef stored_type_signed
#undef stored_type_offset
#undef stored_type_half
#undef stored_type_ref
#undef stored_type_tag
#undef stored_type_loclist
#undef stored_type_address
#undef stored_type_refdie
#undef stored_type_refdie_is_type
#undef stored_type_rangelist
#undef attr_optional
#undef attr_mandatory
#undef super_attr_optional
#undef super_attr_mandatory
#undef child_tag

/****************************************************************/
/* end generated ADT includes                                   */
/****************************************************************/
		/* root_die's name resolution functions */
		template <typename Iter>
		inline iterator_base 
		root_die::resolve(const iterator_base& start, Iter path_pos, Iter path_end)
		{
			if (path_pos == path_end) return start;
			Iter cur_plus_one = path_pos; cur_plus_one++;
			if (cur_plus_one == path_end) return start.named_child(*path_pos);
			else
			{
				auto found = start.named_child(*path_pos);
				if (found == iterator_base::END) return iterator_base::END;
				// HMM -- I *think* this should now be sufficient
				/*auto p_next_hop = 
					std::dynamic_pointer_cast<with_named_children_die>(found);
				if (!p_next_hop) return iterator_base::END;
				else */ return resolve(found, ++path_pos, path_end);
			}			
		}

		inline iterator_base 
		root_die::resolve(const iterator_base& start, const std::string& name)
		{
			std::vector<string> path; path.push_back(name);
			return resolve(start, path.begin(), path.end());
		}

		template <typename Iter>
		inline iterator_base 
		root_die::scoped_resolve(const iterator_base& start, Iter path_pos, Iter path_end)
		{
			std::vector<iterator_base > results;
			scoped_resolve_all(start, path_pos, path_end, results, 1);
			if (results.size() > 0) return *results.begin();
			else return iterator_base::END;
		}

		template <typename Iter>
		inline void
		root_die::scoped_resolve_all(const iterator_base& start, Iter path_pos, Iter path_end, 
			std::vector<iterator_base >& results, int max /*= 0*/) 
		{
			if (max != 0 && results.size() >= max) return;
			auto found_from_here = resolve(path_pos, path_end);
			if (found_from_here) 
			{ 
				results.push_back(found_from_here); 
			}
			if (start.tag_here() == 0) return; // can't recurse
			else // find our nearest encloser that has named children
			{
				auto p_encl = parent(start);
				while (!p_encl.is_a<with_named_children_die>() == 0)
				{
					if (p_encl.tag_here() == 0) { return; } // abort! we ran out of parents
					this->move_to_parent(p_encl);
				}
				// success; continue resolving
				scoped_resolve_all(p_encl, path_pos, path_end, results, max);
				// by definition, we're finished
				return;
			}
		}
		template <typename Iter/* = iterator_df<compile_unit_die>*/ >
		inline Iter root_die::enclosing_cu(const iterator_base& it)
		{ return cu_pos<Iter>(it.get_enclosing_cu_offset()); }

		//// FIXME: do I need this function?
		//inline iterator_base root_die::scoped_resolve(const std::string& name)
		//{ return scoped_resolve(this->begin(), name); }

		inline iterator_base
		with_named_children_die::named_child(const std::string& name, optional_root_arg_decl) const
		{
			/* The default implementation just asks the root. Since we've somehow 
			 * been called via the payload, we have the added inefficiency of 
			 * searching for ourselves first. This shouldn't happen, though 
			 * I'm not sure if we explicitly avoid it. Warn. */
#ifdef DWARFPP_WARN_ON_INEFFICIENT_USAGE
			cerr << "Warning: inefficient usage of with_named_children_die::named_child" << endl;
#endif
			/* NOTE: the idea about payloads knowing about their children is 
			 * already dodgy because it breaks our "no knowledge of structure" 
			 * property. We can likely work around this by requiring that named_child 
			 * implementations call back to the root if they fail, i.e. that the root
			 * is allowed to know about structure which the child doesn't. 
			 * Or we could call into the root to "validate" our view, somehow, 
			 * to support the case where a given root "hides" some DIEs.
			 * This might be a method
			 * 
			 *  iterator_base 
			 *  root_die::check_named_child(const iterator_base& found, const std::string& name)
			 * 
			 * which calls around into find_named_child if the check fails. 
			 * 
			 * i.e. the root_die has a final say, but the payload itself "hints"
			 * at the likely answer. So the payload can avoid find_named_child's
			 * linear search in the common case, but fall back to it in weird
			 * scenarios (deletions). 
			 */
			root_die& r = get_root(opt_r);
			return r.find_named_child(r.find(get_offset()), name); 
		}

		// now compile_unit_die is complete...
		inline basic_die *factory::make_payload(Die::handle_type&& h, root_die& r)
		{
			Die d(std::move(h));
			if (d.tag_here() == DW_TAG_compile_unit) return make_cu_payload(std::move(d.handle), r);
			else return make_non_cu_payload(std::move(d.handle), r);
		}

		inline std::string compile_unit_die::source_file_name(unsigned o) const
		{
			StringList names(d);
			//if (!names) throw Error(current_dwarf_error, 0);
			/* Source file numbers in DWARF are indexed starting from 1. 
			 * Source file zero means "no source file".
			 * However, our array filesbuf is indexed beginning zero! */
			assert(o <= names.get_len()); // FIXME: how to report error? ("throw No_entry();"?)
			return names[o - 1];
		}
		
		//StringList::handle_type compile_unit_die::source_file_names() const
		//{
		//	return StringList::try_construct(get_handle());
		//}
		inline unsigned compile_unit_die::source_file_count() const
		{
			// FIXME: cache some stuff
			StringList names(d);
			return names.get_len();
		}
		
		inline spec& iterator_base::spec_here() const
		{
			if (tag_here() == DW_TAG_compile_unit)
			{
				// we only ask CUs for their spec after payload construction
				assert(state == WITH_PAYLOAD);
				auto p_cu = dynamic_pointer_cast<compile_unit_die>(cur_payload);
				assert(p_cu);
				switch(p_cu->version_stamp)
				{
					case 2: return ::dwarf::spec::dwarf3;
					default: 
						cerr << "Warning: saw unexpected DWARF version stamp " 
							<< p_cu->version_stamp << endl;
						return ::dwarf::spec::dwarf3;
				}
			}
			else return get_handle().get_spec(*p_root);
		}
				
		// make a typedef template to help the client
// 		template <typename Pred, typename DerefAs = basic_die> 
// 		using children_sequence_t
// 		 = pair<
// 			iterator_sibs_where< Pred, DerefAs >,
// 			iterator_sibs_where< Pred, DerefAs >
// 		   >;
// 		template <typename Iter, Dwarf_Half Tag>
// 		struct with_tag
// 		{
// 			bool operator()(const Iter& i) const
// 			{ return i.tag_here() == Tag; }
// 		};		

		
		//template <typename Pred, typename DerefAs = basic_die> 
		//using iterator_sibs_where = boost::filter_iterator< Pred, iterator_sibs<DerefAs> >;

		#define GET_HANDLE_OFFSET \
		Dwarf_Off off; \
			int ret = dwarf_dieoffset(returned, &off, &current_dwarf_error); \
			assert(ret == DW_DLV_OK)
		
		inline Die::handle_type 
		Die::try_construct(root_die& r, const iterator_base& it) /* siblingof */
		{
			raw_handle_type returned;
			auto tmp = dynamic_cast<Die *>(&it.get_handle());
			int ret
			 = dwarf_siblingof(r.dbg.handle.get(), dynamic_cast<Die&>(it.get_handle()).handle.get(), 
			 	&returned, &current_dwarf_error);
			if (ret == DW_DLV_OK)
			{	
				// also update the parent cache
				GET_HANDLE_OFFSET;
				auto found = r.parent_of.find(it.offset_here());
				if (found != r.parent_of.end())
				{
					// parent of the sibling is the same as parent of "it"
					r.parent_of[off] = found->second;
				} else cerr << "Warning: parent cache did not know 0x" << std::hex << it.offset_here() << std::dec << endl;
				return handle_type(returned, deleter(r.dbg.handle.get(), r));
			}
			else return handle_type(nullptr, deleter(nullptr, r));
		}
		inline Die::handle_type 
		Die::try_construct(root_die& r) /* siblingof in root case */
		{
			raw_handle_type returned;
			int ret
			 = dwarf_siblingof(r.dbg.handle.get(), nullptr, &returned, &current_dwarf_error);
			if (ret == DW_DLV_OK)
			{
				// update parent cache
				GET_HANDLE_OFFSET;
				r.parent_of[off] = 0UL;
				return handle_type(returned, deleter(r.dbg.handle.get(), r));
			}
			else return handle_type(nullptr, deleter(nullptr, r));
		}
		inline Die::handle_type 
		Die::try_construct(const iterator_base& it) /* child */
		{
			raw_handle_type returned;
			root_die& r = it.get_root();
			auto tmp0 = &it.get_handle();
			auto tmp = dynamic_cast<core::Die *>(&it.get_handle());
			auto tmp1 = dynamic_cast<core::iterator_base *>(&it.get_handle());
			auto tmp2 = dynamic_cast<core::basic_die *>(&it.get_handle());
			//auto tmp3 = dynamic_cast<core::Die *>(&it.get_handle());
			//auto tmp4 = dynamic_cast<core::Die *>(&it.get_handle());
			int ret = dwarf_child(dynamic_cast<Die&>(it.get_handle()).handle.get(), &returned, &current_dwarf_error);
			if (ret == DW_DLV_OK)
			{
				GET_HANDLE_OFFSET;
				r.parent_of[off] = it.offset_here();
				return handle_type(returned, deleter(it.get_root().dbg.handle.get(), r));
			}
			else return handle_type(nullptr, deleter(nullptr, r));

		}
		inline Die::handle_type 
		Die::try_construct(root_die& r, Dwarf_Off off) /* offdie */
		{
			raw_handle_type returned;
			int ret = dwarf_offdie(r.dbg.handle.get(), off, &returned, &current_dwarf_error);
			if (ret == DW_DLV_OK)
			{
				// can't update parent cache
				return handle_type(returned, deleter(r.dbg.handle.get(), r));
			}
			else return handle_type(nullptr, deleter(nullptr, r));
		}
			
		inline Die::Die(handle_type h) : handle(std::move(h)) {}
		inline Die::Die(root_die& r, const iterator_base& die) /* siblingof */
		 : handle(try_construct(r, die))
		{ 
			if (!this->handle) throw Error(current_dwarf_error, 0); 
		} 
		inline Die::Die(root_die& r) /* siblingof in "first die of CU" case */
		 : handle(try_construct(r))
		{ 
			if (!this->handle) throw Error(current_dwarf_error, 0); 
		} 
		inline Die::Die(root_die& r, Dwarf_Off off) /* offdie */
		 : handle(try_construct(r, off))
		{ 
			if (!this->handle) throw Error(current_dwarf_error, 0); 
		} 
		inline Die::Die(const iterator_base& die) /* child */
		 : handle(try_construct(die))
		{
			if (!this->handle) throw Error(current_dwarf_error, 0); 
		}
		
		inline iterator_df<compile_unit_die>
		iterator_base::enclosing_cu() const
		{ return get_root().pos(enclosing_cu_offset_here(), 1, optional<Dwarf_Off>(0UL)); }
		
		inline spec& Die::spec_here(root_die& r) const
		{
			// HACK: avoid creating any payload for now, for speed-testing
			return ::dwarf::spec::dwarf3;
			
			/* NOTE: subtlety concerning payload construction. To make the 
			 * payload, we need the factory, meaning we need the spec. 
			 * BUT to read the spec, we need the CU payload! 
			 * Escape this by ensuring that root_die::make_payload, in the CU case,
			 * uses the default factory. In turn, we ensure that all factories, 
			 * for the CU case, behave the same. I have refactored
			 * the factory base class so that only non-CU DIEs get the make_payload
			 * call.  */

			// this spec_here() is only used from the factory, and not 
			// used in the CU case because the factory handles that specially
			assert(tag_here() != DW_TAG_compile_unit); 
			
			Dwarf_Off cu_offset = enclosing_cu_offset_here();
			// this should be sticky, hence fast to find
			return r.pos(cu_offset, 1, optional<Dwarf_Off>(0UL)).spec_here();

// 			{
// 				return 
// 				if (state == HANDLE_ONLY)
// 				{
// 					p_root->make_payload(*this);
// 				}
// 				assert(cur_payload && state == WITH_PAYLOAD);
// 				auto p_cu = dynamic_pointer_cast<compile_unit_die>(cur_payload);
// 				assert(p_cu);
// 				switch(p_cu->version_stamp)
// 				{
// 					case 2: return ::dwarf::spec::dwarf3;
// 					default: assert(false);
// 				}
// 			}
// 			else
// 			{
// 				// NO! CUs now lie about their spec
// 				// return nearest_enclosing(DW_TAG_compile_unit).spec_here();
		
		}
		/* NOTE: pos() is incompatible with a strict parent cache.
		 * But it is necessary to support following references.
		 * We fill in the parent if depth <= 2, or if the user can tell us. */
		template <typename Iter /* = iterator_df<> */ >
		inline Iter root_die::pos(Dwarf_Off off, unsigned depth,
			optional<Dwarf_Off> parent_off /* = optional<Dwarf_Off>() */ )
		{
			if (depth == 0) { assert(off == 0UL); return Iter(begin()); }
			auto handle = Die::try_construct(*this, off);
			assert(handle);
			iterator_base base(std::move(handle), depth, *this);
			if (depth == 1) parent_of[off] = 0UL;
			else if (depth == 2) parent_of[off] = base.enclosing_cu_offset_here();
			else if (parent_off) parent_of[off] = *parent_off;
			
			return Iter(std::move(base));
		}
		
		/* We use the properties of diesets to avoid a naive depth-first search. 
		 * FIXME: make it work with encap::-style less strict ordering. 
		 * NOTE: a possible idea here is to support a kind of "fractional offsets"
		 * where we borrow *high-order* bits from the offset space in a dynamic
		 * fashion. We need some per-root bookkeeping about what offsets have
		 * been issued, and a way to get a numerical comparison (for search
		 * functions, like this one). 
		 * Probably the best way to accommodate this is as a new class
		 * used in place of Dwarf_Off. */
		template <typename Iter /* = iterator_df<> */ >
		inline Iter root_die::find(Dwarf_Off off)
		{
			/* Interesting problem: our iterators don't make searching a subtree 
			 * easy. I think there is a neat way of expressing this by combining
			 * dfs and bfs traversal. FIXME: work out the recipe. */
			
			/* I think we want bf traversal with a smart subtree-skipping test. */
			iterator_bf<typename Iter::DerefType> pos = begin();
			while (pos != iterator_base::END && pos.offset_here() != off)
			{
				/* What's next in the breadth-first order? */
				iterator_bf<typename Iter::DerefType> next_pos = pos; next_pos.increment();
				 
				/* Does the move pos->next_pos skip over (enqueue) a subtree? 
				 * If so, the depth will stay the same. 
				 * If no, it's because we took a previously enqueued (deeper, but earlier) 
				 * node out of the queue (i.e. descended instead of skipped-over). */
				if (next_pos != iterator_base::END && next_pos.depth() == pos.depth())
				{
					// if I understand correctly....
					assert(next_pos.offset_here() > pos.offset_here());
					
					/* Might that subtree contain off? */
					if (off < next_pos.offset_here() && off > pos.offset_here())
					{
						/* Yes. We want that subtree. */
						do { pos.increment(); } while (pos.offset_here() > off); //  cut straight to that subtree?
						continue;
					}
					else // off >= next_pos.offset_here() || off <= pos.offset_here()
					{
						/* We can't possibly want that subtree. */
						pos.increment_skipping_subtree();
						continue;
					}
				}
				pos.increment();
			}
			return pos;
		}
		inline Attribute::handle_type 
		Attribute::try_construct(const Die& h, Dwarf_Half attr)
		{
			raw_handle_type returned;
			int ret = dwarf_attr(h.raw_handle(), attr, &returned, &current_dwarf_error);
			if (ret == DW_DLV_OK) return handle_type(returned, deleter(h.get_dbg()));
			else return handle_type(nullptr, deleter(nullptr)); // could be ERROR or NO_ENTRY
		}
		inline Attribute::Attribute(const Die& h, Dwarf_Half attr)
		 : handle(try_construct(h, attr))
		{
			if (!this->handle) throw Error(current_dwarf_error, 0);
		}
		inline AttributeList::handle_type
		AttributeList::try_construct(const Die& h)
		{
			Dwarf_Attribute *block_start;
			Dwarf_Signed count;
			int ret = dwarf_attrlist(h.raw_handle(), &block_start, &count, &current_dwarf_error);
			if (ret == DW_DLV_OK)
			{
				/* Since we are try_construct, the most we can do is 
				 * pass a unique_ptr to the allocated block, where that
				 * unique_ptr's deleter includes the length of the block. */
				assert(count != 0); // this would be ambiguous w.r.t the NO_ENTRY case
				return handle_type(block_start, deleter(h.get_dbg(), count));
			}
			else if (ret == DW_DLV_NO_ENTRY)
			{
				/* We allow zero-length AttributeLists. 
				   HACK: Use (void*)-1 as the block_start. */
				return handle_type((raw_element_type*)-1, deleter(h.get_dbg(), 0));
			}
			else
			{
				return handle_type(nullptr, deleter(nullptr, 0));
			}
		}
		inline AttributeList::AttributeList(const Die& d)
		 : handle(try_construct(d)), d(d)
		{
			if (!handle) throw Error(current_dwarf_error, 0);
			/* Create a unique_ptr to each attribute in the block.
			 * Note: the block contains Dwarf_Attributes, i.e. 
			 * Dwarf_Attribute_s pointers.
			 * We have to take each block element in turn
			 * and make it into an Attribute::handle. */
			copy_list();
		}
		
		inline StringList::handle_type
		StringList::try_construct(const Die& h)
		{
			char **block_start;
			Dwarf_Signed count;
			int ret = dwarf_srcfiles(h.raw_handle(), &block_start, &count, &current_dwarf_error);
			if (ret == DW_DLV_OK)
			{
				assert(count > 0);
				return handle_type(block_start, deleter(h.get_dbg(), count));
			}
			else if (ret == DW_DLV_NO_ENTRY)
			{
				return handle_type((raw_element_type*)-1, deleter(h.get_dbg(), 0));
			}
			else return handle_type(nullptr, deleter(nullptr, 0));
		}
		inline StringList::StringList(const Die& d)
		 : handle(try_construct(d))//, d(d)
		{
			if (!handle) throw Error(current_dwarf_error, 0);
			/* Create a unique_ptr to each attribute in the block.
			 * Note: the block contains char pointers. 
			 * We have to take each block element in turn
			 * and make it into an unique_ptr<char, string_deleter>. */
			copy_list();
		}
		
		
		inline LocdescList::handle_type 
		LocdescList::try_construct(const Attribute& a)
		{
			/* dwarf_loclist_n returns us
			 * a pointer 
			 *   to an array 
			 *      of pointers 
			 *        to Locdescs.
			 * We will copy each pointer in the array into our vector of Locdesc handles. */
			Dwarf_Locdesc **block_start;
			Dwarf_Signed count = 0;
			int ret = dwarf_loclist_n(a.raw_handle(), &block_start, &count, &current_dwarf_error);
			if (ret == DW_DLV_OK)
			{
				assert(count > 0);
				/* Now what? handle_type is a unique_ptr<Dwarf_Locdesc*>
				 * pointing at an array of Dwarf_Locdesc*s . 
				 * We will make unique_ptrs out of each of them, but only when
				 * we upgrade this handle and copy the array. */
				return handle_type(block_start, deleter(a.get_dbg(), count));
			}
			else return handle_type(nullptr, deleter(nullptr, 0));
		}
		inline Dwarf_Unsigned 
		RangeList::get_rangelist_offset(const Attribute& a)
		{
			/* Since DWARF4, form can be unsigned or sec_offset, so we  
			 * check it here. */
			Dwarf_Half form;
			int retF = dwarf_whatform(a.handle.get(), &form, &core::current_dwarf_error); 
			if (retF == DW_DLV_OK)
			{
				int ret;
				switch (form)
				{
					case DW_FORM_udata: {
						Dwarf_Unsigned ranges_off;
						ret = dwarf_formudata(a.handle.get(), &ranges_off, &core::current_dwarf_error); 
						return (ret == DW_DLV_OK) ? ranges_off : (Dwarf_Unsigned)-1;
					}
					case DW_FORM_data4: {
						Dwarf_Signed ref;
						ret = dwarf_formsdata(a.handle.get(), &ref, &core::current_dwarf_error); 
						return (ret == DW_DLV_OK) ? ref : (Dwarf_Unsigned) -1;
					}
					case DW_FORM_sec_offset: {
						Dwarf_Off ref; 
						ret = dwarf_global_formref(a.handle.get(), &ref, &core::current_dwarf_error); 
						return (ret == DW_DLV_OK) ? ref : (Dwarf_Unsigned) -1;
					}
					default: assert(false);
				}
			}
			return (Dwarf_Unsigned)-1; // error
		}
		
		inline RangesList::handle_type 
		RangesList::try_construct(const Attribute& a)
		{
			Dwarf_Unsigned ranges_off = get_rangelist_offset(a);
			if (ranges_off != (Dwarf_Unsigned)-1)
			{
				Dwarf_Ranges *block_start;
				Dwarf_Signed count = 0;
				Dwarf_Unsigned bytes = 0;
				int ret2 = dwarf_get_ranges(a.get_dbg(), ranges_off, &block_start, &count, &bytes, &current_dwarf_error);
				if (ret2 == DW_DLV_OK)
				{
					assert(count > 0);
					/* Now what? handle_type is a unique_ptr<Dwarf_Locdesc*>
					 * pointing at an array of Dwarf_Locdesc*s . 
					 * We will make unique_ptrs out of each of them, but only when
					 * we upgrade this handle and copy the array. */
					return handle_type(block_start, deleter(a.get_dbg(), count));
				}
				else if (ret2 == DW_DLV_NO_ENTRY)
				{
					/* HACK: use (void*)-1 */
					return handle_type((raw_element_type*)-1, deleter(a.get_dbg(), 0));
				}
			}
			return handle_type(nullptr, deleter(nullptr, 0));
		}
		inline RangesList::handle_type 
		RangesList::try_construct(const Attribute& a, const Die& d)
		{
			Dwarf_Unsigned ranges_off = get_rangelist_offset(a);
			if (ranges_off != (Dwarf_Unsigned)-1)
			{
				Dwarf_Ranges *block_start;
				Dwarf_Signed count = 0;
				Dwarf_Unsigned bytes = 0;
				int ret2 = dwarf_get_ranges_a(a.get_dbg(), ranges_off, d.raw_handle(), 
					&block_start, &count, &bytes, &current_dwarf_error);
				if (ret2 == DW_DLV_OK)
				{
					assert(count > 0);
					/* Now what? handle_type is a unique_ptr<Dwarf_Locdesc*>
					 * pointing at an array of Dwarf_Locdesc*s . 
					 * We will make unique_ptrs out of each of them, but only when
					 * we upgrade this handle and copy the array. */
					return handle_type(block_start, deleter(a.get_dbg(), count));
				}
				else if (ret2 == DW_DLV_OK)
				{
					return handle_type((raw_element_type*)-1, deleter(a.get_dbg(), 0));
				}
			}
			
			return handle_type(nullptr, deleter(nullptr, 0));
		}		
		inline Block::handle_type
		Block::try_construct(const Attribute& a)
		{
			Dwarf_Block *returned;
			int ret = dwarf_formblock(a.raw_handle(), &returned, &current_dwarf_error);
			if (ret == DW_DLV_OK)
			{
				return handle_type(returned, deleter(a.get_dbg()));
			} else return handle_type(nullptr, deleter(nullptr));
		}
		
		inline Block::Block(const Attribute& a) : handle(try_construct(a)) 
		{ 
			if (!handle) throw Error(current_dwarf_error, 0);
		}

		// FIXME: what does this constructor do? Can we get rid of it?
		// It seems to be used only for the compile_unit_die constructor.
		//inline basic_die::basic_die(root_die& r/*, const Iter& i*/)
		//: d(Die::handle_type(nullptr)), p_root(&r) {}

		template <typename Iter /* = iterator_df<> */ >
		inline Iter root_die::begin()
		{
			/* The first DIE is always the root.
			 * We denote an iterator pointing at the root by
			 * a Debug but no Die. */
			return Iter(iterator_base(*this));
		}

		template <typename Iter /* = iterator_df<> */ >
		inline Iter root_die::end()
		{
			return Iter(iterator_base::END);
		}

		template <typename Iter /* = iterator_df<> */ >
		inline pair<Iter, Iter> root_die::sequence()
		{
			return std::make_pair(begin(), end());
		}
		
		inline iterator_base::sequence< iterator_sibs<> >
		iterator_base::children_here()
		{
			return std::make_pair<iterator_sibs<>, iterator_sibs<> >(
				p_root->first_child(*this), 
				iterator_base::END
			);
		}
		inline iterator_base::sequence< iterator_sibs<> >
		iterator_base::children_here() const
		{ return const_cast<iterator_base*>(this)->children(); }
		// synonyms
		inline iterator_base::sequence< iterator_sibs<> >
		iterator_base::children() { return children_here(); }
		inline iterator_base::sequence< iterator_sibs<> >
		iterator_base::children() const { return children_here(); }

		// We don't want this because it's putting knowledge of topology
		// into the DIE and not the iterator. 
		// Instead, use r.begin()->children();
// 		inline iterator_sibs<compile_unit_die> root_die::cu_children_begin()
// 		{
// 			return iterator_sibs<compile_unit_die>(first_child(begin()));
// 		}
// 		inline iterator_sibs<compile_unit_die> root_die::cu_children_end()
// 		{
// 			return iterator_base::END;
// 		}
	}
	
	namespace lib
	{
		class file;
		class die;
		class attribute;
		class attribute_array;
		class global_array;
		class global;
		class block;
		class loclist;
        class aranges;
        class ranges;
		class evaluator;
		class srclines;
		
		class basic_die;
		class file_toplevel_die;
		
		struct No_entry {
			No_entry() {}
		};	

		void default_error_handler(Dwarf_Error error, Dwarf_Ptr errarg); 
		class file {
			friend class die;
			friend class global_array;
			friend class attribute_array;
			friend class aranges;
			friend class ranges;
			friend class srclines;
			friend class srcfiles;

			friend class file_toplevel_die;
			friend class compile_unit_die;

			int fd;
			Dwarf_Debug dbg; // our peer structure
			Dwarf_Error last_error; // pointer to Dwarf_Error_s detailing our last error
			//dieset file_ds; // the structure to hold encapsulated DIEs, if we use it

			/*dwarf_elf_handle*/ Elf* elf;
			bool free_elf; // whether to do elf_end in destructor

			aranges *p_aranges;

			// public interfaces to these are to use die constructors
			int siblingof(die& d, die *return_sib, Dwarf_Error *error = 0);
			int first_die(die *return_die, Dwarf_Error *error = 0); // special case of siblingof

		protected:
			bool have_cu_context; 

			/* We have a default constructor so that 
			 * - encap::file can inherit from us
			 * - we can wrap the libdwarf producer interface too.
			 * Note that dummy_file has gone away! */
			// protected constructor
			file() : fd(-1), dbg(0), last_error(0), 
			  elf(0), p_aranges(0), have_cu_context(false)
			{} 

			// we call out to a function like this when we hit a CU in clear_cu_context
			typedef void (*cu_callback_t)(void *arg, 
				Dwarf_Off cu_offset,
				Dwarf_Unsigned cu_header_length,
				Dwarf_Half version_stamp,
				Dwarf_Unsigned abbrev_offset,
				Dwarf_Half address_size,
				Dwarf_Unsigned next_cu_header);
				
            // libdwarf has a weird stateful API for getting compile unit DIEs.
            int clear_cu_context(cu_callback_t cb = 0, void *arg = 0);

			// TODO: forbid copying or assignment by adding private definitions 
		public:
			int get_fd() { return fd; }
			Dwarf_Debug get_dbg() { return dbg; }
			//dieset& get_ds() { return file_ds; }
			file(int fd, Dwarf_Unsigned access = DW_DLC_READ,
				Dwarf_Ptr errarg = 0,
				Dwarf_Handler errhand = default_error_handler,
				Dwarf_Error *error = 0);

			virtual ~file();

			int advance_cu_context(Dwarf_Unsigned *cu_header_length = 0,
				Dwarf_Half *version_stamp = 0,
				Dwarf_Unsigned *abbrev_offset = 0,
				Dwarf_Half *address_size = 0, 
				Dwarf_Unsigned *next_cu_header = 0,
				cu_callback_t cb = 0, void *arg = 0);
			
			/* map a function over all CUs */
			void iterate_cu(cu_callback_t cb, void *arg = 0)
			{
				clear_cu_context(); 
				while (advance_cu_context(0, 0, 0, 0, 0, cb, arg) == DW_DLV_OK);
			}
			
			int ensure_cu_context()
			{
				if (have_cu_context) return DW_DLV_OK;
				else 
				{
					int retval = advance_cu_context();
					assert(retval != DW_DLV_OK || have_cu_context);
					return retval; 
				}
			}
			
		protected:
			// public interface to this: use advance_cu_context
			int next_cu_header(
				Dwarf_Unsigned *cu_header_length,
				Dwarf_Half *version_stamp,
				Dwarf_Unsigned *abbrev_offset,
				Dwarf_Half *address_size,
				Dwarf_Unsigned *next_cu_header,
				Dwarf_Error *error = 0);
		private:
			int offdie(Dwarf_Off offset, die *return_die, Dwarf_Error *error = 0);
		public:

	//		int get_globals(
	//			global_array *& globarr, // on success, globarr points to a global_array
	//			Dwarf_Error *error = 0);

			int get_cu_die_offset_given_cu_header_offset(
				Dwarf_Off in_cu_header_offset,
				Dwarf_Off * out_cu_die_offset,
				Dwarf_Error *error = 0);
                
            int get_elf(Elf **out_elf, Dwarf_Error *error = 0);
            
            aranges& get_aranges() { if (p_aranges) return *p_aranges; else throw No_entry(); }
		};

		class die {
			friend class file;
			friend class attribute_array;
			friend class dwarf::encap::die;
			friend class block;
			friend class loclist;
			friend class ranges;
			friend class srclines;
			friend class srcfiles;
			
			friend class basic_die;
			friend class file_toplevel_die;
		public: // HACK while I debug the null-f bug
			file& f;
		private:
			Dwarf_Error *const p_last_error;
			Dwarf_Die my_die;
			die(file& f, Dwarf_Die d, Dwarf_Error *perror);
			Dwarf_Die get_die() { return my_die; }

			// public interface is to use constructor
			int first_child(die *return_kid, Dwarf_Error *error = 0);
			
			// "uninitialized" DIE is used to back file_toplevel_die
			//static file dummy_file;
			// get rid of this ^^! basic_die now no longer needs it
			
			bool have_cu_context; 

		public:
			virtual ~die();
			die(file& f, int dummy) : f(f), p_last_error(&f.last_error)
			{
				/* This constructor is used for the dummy DIE representing the
				 * top of the tree. We set my_die to 0 (it's a pointer). */
				my_die = 0;
			}
			die(file& f) : f(f), p_last_error(&f.last_error)
				{	/* ask the file to initialize us with the first DIE of the current CU */
					int cu_retval = f.ensure_cu_context();
					if (cu_retval != DW_DLV_OK) throw Error(*p_last_error, f.get_dbg());
					int retval = f.first_die(this, p_last_error);
					if (retval == DW_DLV_NO_ENTRY) throw No_entry();
					else if (retval == DW_DLV_ERROR) throw Error(*p_last_error, f.get_dbg()); }
			die(file& f, const die& d) : f(f), p_last_error(&f.last_error)
				{	/* ask the file to initialize us with the next sibling of d */
					int retval = f.siblingof(const_cast<die&>(d), this, p_last_error);
					if (retval == DW_DLV_NO_ENTRY) throw No_entry(); 
					else if (retval == DW_DLV_ERROR) throw Error(*p_last_error, f.get_dbg()); }
			// this is *not* a copy constructor! it constructs the child
			explicit die(const die& d) : f(const_cast<die&>(d).f), p_last_error(&f.last_error)
				{	/* ask the file to initialize us with the first child of d */
					int retval = const_cast<die&>(d).first_child(this, p_last_error); 
					if (retval == DW_DLV_NO_ENTRY) throw No_entry(); 
					else if (retval == DW_DLV_ERROR) throw Error(*p_last_error, f.get_dbg()); }
			die(file& f, Dwarf_Off off) : f(f), p_last_error(&f.last_error)
				{	/* ask the file to initialize us with a DIE from a known offset */
					assert (off != 0UL); // file_toplevel_die should use protected constructor
					int retval = f.offdie(off, this, p_last_error);
					if (retval == DW_DLV_ERROR) throw Error(*p_last_error, f.get_dbg()); }			

			int tag(Dwarf_Half *tagval,	Dwarf_Error *error = 0) const;
			int offset(Dwarf_Off * return_offset, Dwarf_Error *error = 0) const;
			int CU_offset(Dwarf_Off *return_offset, Dwarf_Error *error = 0);
			int name(string *return_name, Dwarf_Error *error = 0) const;
			//int attrlist(attribute_array *& attrbuf, Dwarf_Error *error = 0);
			int hasattr(Dwarf_Half attr, Dwarf_Bool *return_bool, Dwarf_Error *error = 0);
			int hasattr(Dwarf_Half attr, Dwarf_Bool *return_bool, Dwarf_Error *error = 0) const
			{ return const_cast<die *>(this)->hasattr(attr, return_bool, error); }
			//int attr(Dwarf_Half attr, attribute *return_attr, Dwarf_Error *error = 0);
			int lowpc(Dwarf_Addr * return_lowpc, Dwarf_Error * error = 0);
			int highpc(Dwarf_Addr * return_highpc, Dwarf_Error *error = 0);
			int bytesize(Dwarf_Unsigned *return_size, Dwarf_Error *error = 0);
			int bitsize(Dwarf_Unsigned *return_size, Dwarf_Error *error = 0);
			int bitoffset(Dwarf_Unsigned *return_size, Dwarf_Error *error = 0);
			int srclang(Dwarf_Unsigned *return_lang, Dwarf_Error *error = 0);
			int arrayorder(Dwarf_Unsigned *return_order, Dwarf_Error *error = 0);
		};

		/* Attributes may be freely passed by value, because there is 
         * no libdwarf resource allocation done when getting attributes
         * (it's all done when getting the attribute array). 
		 * In other words, an attribute is just a <base, offset> pair
		 * denoting a position in an attribute_array, which is assumed
		 * to outlive the attribute itself. */
		class attribute {
			friend class die;
			friend class attribute_array;
			friend class block;
			friend class loclist;
            friend class ranges;
			core::Attribute::raw_handle_type p_raw_attr;
			attribute_array *p_a;
			int i;
			core::Debug::raw_handle_type p_raw_dbg;
		public:
			inline attribute(attribute_array& a, int i); // defined in a moment
			attribute(Dwarf_Half attr, attribute_array& a, Dwarf_Error *error = 0);
			attribute(const core::Attribute& a, core::Debug::raw_handle_type dbg)
			 : p_raw_attr(a.handle.get()), p_a(0), i(0), p_raw_dbg(dbg) {}
			virtual ~attribute() {}	

			public:
			int hasform(Dwarf_Half form, Dwarf_Bool *return_hasform, Dwarf_Error *error = 0) const;
			int whatform(Dwarf_Half *return_form, Dwarf_Error *error = 0) const;
			int whatform_direct(Dwarf_Half *return_form, Dwarf_Error *error = 0) const;
			int whatattr(Dwarf_Half *return_attr, Dwarf_Error *error = 0) const;
			int formref(Dwarf_Off *return_offset, Dwarf_Error *error = 0) const;
			int formref_global(Dwarf_Off *return_offset, Dwarf_Error *error = 0) const;
			int formaddr(Dwarf_Addr * return_addr, Dwarf_Error *error = 0) const;
			int formflag(Dwarf_Bool * return_bool, Dwarf_Error *error = 0) const;
			int formudata(Dwarf_Unsigned * return_uvalue, Dwarf_Error * error = 0) const;
			int formsdata(Dwarf_Signed * return_svalue, Dwarf_Error *error = 0) const;
			int formblock(Dwarf_Block ** return_block, Dwarf_Error * error = 0) const;
			int formstring(char ** return_string, Dwarf_Error *error = 0) const;
			int loclist_n(Dwarf_Locdesc ***llbuf, Dwarf_Signed *listlen, Dwarf_Error *error = 0) const;
			int loclist(Dwarf_Locdesc **llbuf, Dwarf_Signed *listlen, Dwarf_Error *error = 0) const;

			const attribute_array& get_containing_array() const { return *p_a; }
		};

		class attribute_array {
			friend class die;
			friend class attribute;
			friend class block;
			friend class loclist;
            friend class ranges; 
			Dwarf_Attribute *p_attrs;
			die& d;
			Dwarf_Error *const p_last_error;
			Dwarf_Signed cnt;
			// TODO: forbid copying or assignment by adding private definitions 
		public:
			//attribute_array(Dwarf_Debug dbg, Dwarf_Attribute *attrs, Dwarf_Signed cnt)
			//	: dbg(dbg), attrs(attrs), cnt(cnt) {}
			attribute_array(die &d, Dwarf_Error *error = 0) : 
				d(d), p_last_error(error ? error : &d.f.last_error)
			{
				if (error == 0) error = p_last_error;
				int retval = dwarf_attrlist(d.get_die(), &p_attrs, &cnt, error);
				if (retval == DW_DLV_NO_ENTRY)
				{
					// this means two things -- cnt is zero, and nothing is allocated
					p_attrs = 0;
					cnt = 0;
				}
				else if (retval != DW_DLV_OK) 
				{
					throw Error(*error, d.f.get_dbg());
				}
			}
			Dwarf_Signed count() { return cnt; }
			attribute get(int i) { return attribute(*this, i); }
            attribute operator[](Dwarf_Half attr) { return attribute(attr, *this); }

			virtual ~attribute_array()	{
				for (int i = 0; i < cnt; i++)
				{
					dwarf_dealloc(d.f.dbg, p_attrs[i], DW_DLA_ATTR);
				}
				if (p_attrs != 0) dwarf_dealloc(d.f.dbg, p_attrs, DW_DLA_LIST);
			}
			const die& get_containing_die() const { return d; }
		};
		
		inline attribute::attribute(attribute_array& a, int i) : p_a(&a), i(i)
		{ p_raw_attr = p_a->p_attrs[i]; }
		
		class block {
			attribute attr; // it's okay to pass attrs by value
			Dwarf_Block *b;
		public:
			block(attribute a, Dwarf_Error *error = 0) : attr(a) // it's okay to pass attrs by value
			{
				if (error == 0) error = attr.p_a->p_last_error;
				int retval = a.formblock(&b);
				if (retval != DW_DLV_OK) throw Error(*error, attr.p_a->d.f.get_dbg());
			}

			Dwarf_Unsigned len() { return b->bl_len; }
			Dwarf_Ptr data() { return b->bl_data; }

			virtual ~block() { dwarf_dealloc(attr.p_a->d.f.get_dbg(), b, DW_DLA_BLOCK); }
		};
		
		ostream& operator<<(ostream& s, const Dwarf_Locdesc& ld);	
		ostream& operator<<(ostream& s, const Dwarf_Loc& l);
		ostream& operator<<(ostream& s, const loclist& ll);
		
		class global {
			friend class global_array;
			global_array& a;
			int i;

 			global(global_array& a, int i) : a(a), i(i) {}

			public:
			int get_name(char **return_name, Dwarf_Error *error = 0);
			int get_die_offset(Dwarf_Off *return_offset, Dwarf_Error *error = 0);
			int get_cu_offset(Dwarf_Off *return_offset, Dwarf_Error *error = 0);
			int cu_die_offset_given_cu_header_offset(Dwarf_Off in_cu_header_offset,
				Dwarf_Off *out_cu_die_offset, Dwarf_Error *error = 0);
			int name_offsets(char **return_name, Dwarf_Off *die_offset, 
				Dwarf_Off *cu_offset, Dwarf_Error *error = 0);			
		};

		class global_array {
		friend class file;
		friend class global;
			file& f;
			Dwarf_Error *const p_last_error;
			Dwarf_Global *p_globals;
			Dwarf_Debug dbg;
			Dwarf_Signed cnt;
			// TODO: forbid copying or assignment by adding private definitions 
		public:
			//global_array(Dwarf_Debug dbg, Dwarf_Global *globals, Dwarf_Signed cnt);
			global_array(file& f, Dwarf_Error *error = 0) : f(f), p_last_error(error ? error : &f.last_error)
			{
				if (error == 0) error = p_last_error;
				int retval = dwarf_get_globals(f.get_dbg(), &p_globals, &cnt, error);
				if (retval != DW_DLV_OK) throw Error(*error, f.get_dbg());
			}
			Dwarf_Signed count() { return cnt; }		
			global get(int i) { return global(*this, i); }

			virtual ~global_array() { dwarf_globals_dealloc(f.get_dbg(), p_globals, cnt); }
		};
        
        class aranges
        {
        	file &f;
		public: // temporary HACK
            Dwarf_Error *const p_last_error;
		//private:
            Dwarf_Arange *p_aranges;
            Dwarf_Signed cnt;
            // TODO: forbid copying or assignment
        public:
        	aranges(file& f, Dwarf_Error *error = 0) : f(f), p_last_error(error ? error : &f.last_error)
            {
            	if (error == 0) error = p_last_error;
                int retval = dwarf_get_aranges(f.get_dbg(), &p_aranges, &cnt, error);
                if (retval == DW_DLV_NO_ENTRY) { cnt = 0; p_aranges = 0; }
                else if (retval != DW_DLV_OK) throw Error(*error, f.get_dbg());
				cerr << "Constructed an aranges from block at " << p_aranges 
					<< ", count " << cnt << endl;
            }
			Dwarf_Signed count() { return cnt; }		
			int get_info(int i, Dwarf_Addr *start, Dwarf_Unsigned *length, Dwarf_Off *cu_die_offset,
				Dwarf_Error *error = 0);
			int get_info_for_addr(Dwarf_Addr a, Dwarf_Addr *start, Dwarf_Unsigned *length, Dwarf_Off *cu_die_offset,
				Dwarf_Error *error = 0);
			void *arange_block_base() const { return p_aranges; }
            virtual ~aranges()
            {
            	// FIXME: uncomment this after dwarf_dealloc segfault bug fixed
                
            	//for (int i = 0; i < cnt; ++i) dwarf_dealloc(f.dbg, p_aranges[i], DW_DLA_ARANGE);
				//dwarf_dealloc(f.dbg, p_aranges, DW_DLA_LIST);
            }
        };

        class ranges
        {
            const die& d;
            Dwarf_Error *const p_last_error;
            Dwarf_Ranges *p_ranges;
            Dwarf_Signed cnt;
            // TODO: forbid copying or assignment
        public:
        	ranges(const attribute& a, Dwarf_Off range_off, Dwarf_Error *error = 0) 
            : d(a.p_a->d), p_last_error(error ? error : &d.f.last_error)
            {
            	if (error == 0) error = p_last_error;
                Dwarf_Unsigned bytes;
                int res;
                res = dwarf_get_ranges_a(d.f.dbg, range_off, 
                	d.my_die, &p_ranges, &cnt, &bytes, error);
                if (res == DW_DLV_NO_ENTRY) throw No_entry();
                assert(res == DW_DLV_OK);
            }
            
            Dwarf_Ranges operator[](Dwarf_Signed i)
            { assert(i < cnt); return p_ranges[i]; }
            Dwarf_Ranges *begin() { return p_ranges; }
            Dwarf_Ranges *end() { return p_ranges + cnt; }
            
			Dwarf_Signed count() { return cnt; }		
            virtual ~ranges()
            {
            	dwarf_ranges_dealloc(d.f.dbg, p_ranges, cnt);
            }
        };
		
		class srclines
		{
			const file& f;
			Dwarf_Line *linebuf;
			Dwarf_Signed linecount;
			Dwarf_Error *const p_last_error;
		
		public:
			srclines(die& d, Dwarf_Error *error = 0) : f(d.f), 
				p_last_error(error ? error : &d.f.last_error)
			{
				linecount = -1;
				if (error == 0) error = p_last_error;
				int ret = dwarf_srclines(d.my_die, &linebuf, &linecount, error);
				if (ret == DW_DLV_OK) linecount = -1; // don't deallocate anything
				assert(ret == DW_DLV_OK);
			}
			
			virtual ~srclines()
			{
				if (linecount != -1 /*&& &d.f != &die::dummy_file*/)
				{
					dwarf_srclines_dealloc(f.dbg, linebuf, linecount);
				}
			}
		};

		class srcfiles
		{
			const file& f;
			char **filesbuf;
			Dwarf_Signed filescount;
			Dwarf_Error *const p_last_error;
		
		public:
			srcfiles(die& d, Dwarf_Error *error = 0) : f(d.f),
				p_last_error(error ? error : &d.f.last_error)
			{
				filescount = -1;
				if (error == 0) error = p_last_error;
				int ret = dwarf_srcfiles(d.my_die, &filesbuf, &filescount, error);
				if (ret == DW_DLV_NO_ENTRY) filescount = -1; // don't deallocate anything
				assert(ret == DW_DLV_OK);
			}
			
			virtual ~srcfiles()
			{
				if (filescount != -1 /*&& &d.f != &die::dummy_file*/) 
				{
					for (int i = 0; i < filescount; ++i)
					{
						dwarf_dealloc(f.dbg, filesbuf[i], DW_DLA_STRING);
					}
					dwarf_dealloc(f.dbg, filesbuf, DW_DLA_LIST);
				}
			}
			
			std::string get(unsigned o)
			{
				/* Source file numbers in DWARF are indexed starting from 1. 
				 * Source file zero means "no source file".
				 * However, our array filesbuf is indexed beginning zero! */
				if (o >= 1 && o <= filescount) return filesbuf[o-1];
				else throw No_entry();
			}
			
			unsigned count() { return filescount; }
		};		
		class Not_supported
        {
        	const string& m_msg;
        public:
        	Not_supported(const string& msg) : m_msg(msg) {}
        };
        
		class regs
        {
        public:
        	virtual Dwarf_Signed get(int regnum) = 0;
            virtual void set(int regnum, Dwarf_Signed val) 
            { throw Not_supported("writing registers"); }
		};

		class evaluator {
			std::stack<Dwarf_Unsigned> m_stack;
			vector<Dwarf_Loc> expr;
			const ::dwarf::spec::abstract_def& spec;
			regs *p_regs; // optional set of register values, for DW_OP_breg*
			boost::optional<Dwarf_Signed> frame_base;
			vector<Dwarf_Loc>::iterator i;
			void eval();
		public:
			evaluator(const vector<unsigned char> expr, 
				const ::dwarf::spec::abstract_def& spec) : spec(spec), p_regs(0)
			{
				//i = expr.begin();
				assert(false);
			}
			evaluator(const encap::loclist& loclist,
				Dwarf_Addr vaddr,
				const ::dwarf::spec::abstract_def& spec = spec::DEFAULT_DWARF_SPEC,
				regs *p_regs = 0,
				boost::optional<Dwarf_Signed> frame_base = boost::optional<Dwarf_Signed>(),
				const stack<Dwarf_Unsigned>& initial_stack = stack<Dwarf_Unsigned>()); 
			
			evaluator(const vector<Dwarf_Loc>& loc_desc,
				const ::dwarf::spec::abstract_def& spec,
				const stack<Dwarf_Unsigned>& initial_stack = stack<Dwarf_Unsigned>()) 
				: m_stack(initial_stack), spec(spec), p_regs(0)
			{
				expr = loc_desc;
				i = expr.begin();
				eval();
			}
			evaluator(const vector<Dwarf_Loc>& loc_desc,
				const ::dwarf::spec::abstract_def& spec,
				regs& regs,
				Dwarf_Signed frame_base,
				const stack<Dwarf_Unsigned>& initial_stack = stack<Dwarf_Unsigned>()) 
				: m_stack(initial_stack), spec(spec), p_regs(&regs)
			{
				expr = loc_desc;
				i = expr.begin();
				this->frame_base = frame_base;
				eval();
			}

			evaluator(const vector<Dwarf_Loc>& loc_desc,
				const ::dwarf::spec::abstract_def& spec,
				Dwarf_Signed frame_base,
				const stack<Dwarf_Unsigned>& initial_stack = stack<Dwarf_Unsigned>()) 
				: m_stack(initial_stack), spec(spec), p_regs(0)
			{
				//if (av.get_form() != dwarf::encap::attribute_value::LOCLIST) throw "not a DWARF expression";
				//if (av.get_loclist().size() != 1) throw "only support singleton loclists for now";			
				//expr = *(av.get_loclist().begin());
				expr = loc_desc;
				i = expr.begin();
				this->frame_base = frame_base;
				eval();
			}
			
			Dwarf_Unsigned tos() const { return m_stack.top(); }
			bool finished() const { return i == expr.end(); }
			Dwarf_Loc current() const { return *i; }
		};
		Dwarf_Unsigned eval(const encap::loclist& loclist,
			Dwarf_Addr vaddr,
			Dwarf_Signed frame_base,
			boost::optional<regs&> rs,
			const ::dwarf::spec::abstract_def& spec,
			const stack<Dwarf_Unsigned>& initial_stack);
		
		class loclist {
			attribute attr;
			Dwarf_Locdesc **locs;
			Dwarf_Signed locs_len;
			friend ostream& operator<<(ostream&, const loclist&);
			friend struct ::dwarf::encap::loclist;
		public:
			loclist(attribute a, Dwarf_Error *error = 0) : attr(a)
			{
				if (error == 0) error = attr.p_a->p_last_error;
				
				int retval = a.loclist_n(&locs, &locs_len);
				if (retval == DW_DLV_NO_ENTRY)
				{
					// this means two things -- locs_len is zero, and nothing is allocated
					locs = 0;
					locs_len = 0;
					
				}
				else if (retval == DW_DLV_ERROR) throw Error(*error, attr.p_a->d.f.get_dbg());
				else assert(retval == DW_DLV_OK);
				//cerr << "Constructed a lib::loclist at " << this << ", len is " << locs_len << endl;
			}
			loclist(const core::Attribute& a, core::Debug::raw_handle_type dbg) : attr(a, dbg)
			{
				//if (error == 0) error = attr.a.p_last_error;
				int retval = dwarf_loclist_n(a.handle.get(), &locs, &locs_len, &core::current_dwarf_error);
				if (retval == DW_DLV_NO_ENTRY)
				{
					// this means two things -- locs_len is zero, and nothing is allocated
					locs = 0;
					locs_len = 0;
				}
				else if (retval == DW_DLV_ERROR) throw Error(core::current_dwarf_error, 0);
				else assert(retval == DW_DLV_OK);
				//cerr << "Constructed a lib::loclist at " << this << ", len is " << locs_len << endl;
			}

			Dwarf_Signed len() const { return locs_len; }
			const Dwarf_Locdesc& operator[] (size_t off) const { return *locs[off]; }

			virtual ~loclist()
			{
				//cerr << "Destructing a lib::loclist (len " << locs_len << ") at " << this << endl;
				for (int i = 0; i < locs_len; ++i) 
				{
					//cerr << "dwarf_deallocing " << locs[i]->ld_s << endl;
					dwarf_dealloc(attr.p_raw_dbg, locs[i]->ld_s, DW_DLA_LOC_BLOCK);
					//cerr << "dwarf_deallocing " << locs[i] << endl;
					dwarf_dealloc(attr.p_raw_dbg, locs[i], DW_DLA_LOCDESC);
				}
				if (locs != 0) 
				{
					//cerr << "dwarf_deallocing " << locs << endl;
					dwarf_dealloc(attr.p_raw_dbg, locs, DW_DLA_LIST);
				}
				//cerr << "Destructed a lib::loclist (len " << locs_len << ") at " << this << endl;
			}
		};
		ostream& operator<<(std::ostream& s, const loclist& ll);	

		bool operator==(const Dwarf_Loc& e1, const Dwarf_Loc& e2);
		bool operator!=(const Dwarf_Loc& e1, const Dwarf_Loc& e2);
	} // end namespace lib
} // end namespace dwarf

#endif
