// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*- 
// vim: ts=8 sw=2 smarttab
/*
 * Ceph - scalable distributed file system
 *
 * Copyright (C) 2004-2006 Sage Weil <sage@newdream.net>
 *
 * This is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License version 2.1, as published by the Free Software 
 * Foundation.  See file COPYING.
 * 
 */


#ifndef CEPH_FILER_H
#define CEPH_FILER_H

/*** Filer
 *
 * stripe file ranges onto objects.
 * build list<ObjectExtent> for the objecter or objectcacher.
 *
 * also, provide convenience methods that call objecter for you.
 *
 * "files" are identified by ino. 
 */

#include "include/types.h"

#include "osd/OSDMap.h"
#include "Objecter.h"

class Context;
class Messenger;
class OSDMap;



/**** Filer interface ***/

class Filer {
  CephContext *cct;
  Objecter   *objecter;
  
  // probes
  struct Probe {
    inodeno_t ino;
    ceph_file_layout layout;
    snapid_t snapid;

    uint64_t *psize;
    utime_t *pmtime;

    int flags;

    bool fwd;

    Context *onfinish;
    
    vector<ObjectExtent> probing;
    uint64_t probing_off, probing_len;
    
    map<object_t, uint64_t> known_size;
    utime_t max_mtime;

    set<object_t> ops;

    int err;
    bool found_size;

    Probe(inodeno_t i, ceph_file_layout &l, snapid_t sn,
	  uint64_t f, uint64_t *e, utime_t *m, int fl, bool fw, Context *c) : 
      ino(i), layout(l), snapid(sn),
      psize(e), pmtime(m), flags(fl), fwd(fw), onfinish(c),
      probing_off(f), probing_len(0),
      err(0), found_size(false) {}
  };
  
  class C_Probe;

  void _probe(Probe *p);
  void _probed(Probe *p, const object_t& oid, uint64_t size, utime_t mtime);

 public:
  Filer(const Filer& other);
  const Filer operator=(const Filer& other);

  Filer(Objecter *o) : cct(o->cct), objecter(o) {}
  ~Filer() {}

  bool is_active() {
    return objecter->is_active(); // || (oc && oc->is_active());
  }


  /***** mapping *****/

  /*
   * map (ino, layout, offset, len) to a (list of) OSDExtents (byte
   * ranges in objects on (primary) osds)
   */
  static void file_to_extents(CephContext *cct, const char *object_format,
			      ceph_file_layout *layout,
			      uint64_t offset, uint64_t len,
			      vector<ObjectExtent>& extents,
			      uint64_t buffer_offset=0);

  static void file_to_extents(CephContext *cct, inodeno_t ino,
			      ceph_file_layout *layout,
			      uint64_t offset, uint64_t len,
			      vector<ObjectExtent>& extents) {
    // generate prefix/format
    char buf[32];
    snprintf(buf, sizeof(buf), "%llx.%%08llx", (long long unsigned)ino);

    file_to_extents(cct, buf, layout, offset, len, extents);
  }

  /**
   * reverse map an object extent to file extents
   */
  static void extent_to_file(CephContext *cct, ceph_file_layout *layout,
			     uint64_t objectno, uint64_t off, uint64_t len,
			     vector<pair<uint64_t, uint64_t> >& extents);

  /*
   * helper to assemble a striped result
   */
  class StripedReadResult {
    map<uint64_t, pair<bufferlist, uint64_t> > partial;  // offset -> (data, intended length)

  public:
    void add_partial_result(bufferlist& bl,
			    const vector<pair<uint64_t,uint64_t> >& buffer_extents) {
      for (vector<pair<uint64_t,uint64_t> >::const_iterator p = buffer_extents.begin();
	   p != buffer_extents.end();
	   ++p) {
	pair<bufferlist, uint64_t>& r = partial[p->first];
	size_t actual = MIN(bl.length(), p->second);
	bl.splice(0, actual, &r.first);
	r.second = p->second;
      }
    }

    /**
     * add sparse read into results
     *
     * @param bl buffer
     * @param bl_map map of which logical source extents this covers
     * @param bl_off logical buffer offset (e.g., first bl_map key if the buffer is not sparse)
     * @param buffer_extents output buffer extents the data maps to
     */
    void add_partial_sparse_result(bufferlist& bl, const map<uint64_t, uint64_t>& bl_map,
				   uint64_t bl_off,
				   const vector<pair<uint64_t,uint64_t> >& buffer_extents) {
      map<uint64_t, uint64_t>::const_iterator s = bl_map.begin();
      for (vector<pair<uint64_t,uint64_t> >::const_iterator p = buffer_extents.begin();
	   p != buffer_extents.end();
	   ++p) {
	uint64_t tofs = p->first;
	uint64_t tlen = p->second;
	while (tlen > 0) {
	  if (s == bl_map.end()) {
	    pair<bufferlist, uint64_t>& r = partial[tofs];
	    r.second = tlen;
	    break;
	  }

	  // skip zero-length extent
	  if (s->second == 0) {
	    s++;
	    continue;
	  }

	  if (s->first > bl_off) {
	    // gap in sparse read result
	    pair<bufferlist, uint64_t>& r = partial[tofs];
	    size_t gap = s->first - bl_off;
	    r.second = gap;
	    bl_off += gap;
	    tofs += gap;
	    tlen -= gap;
	  }

	  assert(s->first <= bl_off);
	  size_t left = (s->first + s->second) - bl_off;
	  size_t actual = MIN(left, tlen);

	  pair<bufferlist, uint64_t>& r = partial[tofs];
	  bl.splice(0, actual, &r.first);
	  r.second = actual;
	  bl_off += actual;
	  tofs += actual;
	  tlen -= actual;

	  if (actual == left)
	    s++;
	}
	bl_off += p->second;
      }
    }

    void assemble_result(bufferlist& bl, bool zero_tail) {
      // go backwards, so that we can efficiently discard zeros
      map<uint64_t,pair<bufferlist,uint64_t> >::reverse_iterator p = partial.rbegin();
      if (p == partial.rend())
	return;

      uint64_t end = p->first + p->second.second;
      while (p != partial.rend()) {
	// sanity check
	assert(p->first == end - p->second.second);
	end = p->first;

	size_t len = p->second.first.length();
	if (len < p->second.second) {
	  if (zero_tail || bl.length()) {
	    bufferptr bp(p->second.second - p->second.first.length());
	    bp.zero();
	    bl.push_front(bp);
	    bl.claim_prepend(p->second.first);
	  } else {
	    bl.claim_prepend(p->second.first);
	  }
	} else {
	  bl.claim_prepend(p->second.first);
	}
	p++;
      }
      partial.clear();
    }
  };


  /*** async file interface.  scatter/gather as needed. ***/

  int read(inodeno_t ino,
	   ceph_file_layout *layout,
	   snapid_t snap,
           uint64_t offset, 
           uint64_t len, 
           bufferlist *bl,   // ptr to data
	   int flags,
           Context *onfinish) {
    assert(snap);  // (until there is a non-NOSNAP write)
    vector<ObjectExtent> extents;
    file_to_extents(cct, ino, layout, offset, len, extents);
    objecter->sg_read(extents, snap, bl, flags, onfinish);
    return 0;
  }

  int read_trunc(inodeno_t ino,
	   ceph_file_layout *layout,
	   snapid_t snap,
           uint64_t offset, 
           uint64_t len, 
           bufferlist *bl,   // ptr to data
	   int flags,
	   uint64_t truncate_size,
	   __u32 truncate_seq,
           Context *onfinish) {
    assert(snap);  // (until there is a non-NOSNAP write)
    vector<ObjectExtent> extents;
    file_to_extents(cct, ino, layout, offset, len, extents);
    objecter->sg_read_trunc(extents, snap, bl, flags,
			    truncate_size, truncate_seq, onfinish);
    return 0;
  }

  int write(inodeno_t ino,
	    ceph_file_layout *layout,
	    const SnapContext& snapc,
	    uint64_t offset, 
            uint64_t len, 
            bufferlist& bl,
	    utime_t mtime,
            int flags, 
            Context *onack,
            Context *oncommit) {
    vector<ObjectExtent> extents;
    file_to_extents(cct, ino, layout, offset, len, extents);
    objecter->sg_write(extents, snapc, bl, mtime, flags, onack, oncommit);
    return 0;
  }

  int write_trunc(inodeno_t ino,
	    ceph_file_layout *layout,
	    const SnapContext& snapc,
	    uint64_t offset, 
            uint64_t len, 
            bufferlist& bl,
	    utime_t mtime,
            int flags, 
	   uint64_t truncate_size,
	   __u32 truncate_seq,
            Context *onack,
            Context *oncommit) {
    vector<ObjectExtent> extents;
    file_to_extents(cct, ino, layout, offset, len, extents);
    objecter->sg_write_trunc(extents, snapc, bl, mtime, flags,
		       truncate_size, truncate_seq, onack, oncommit);
    return 0;
  }

  int truncate(inodeno_t ino,
	       ceph_file_layout *layout,
	       const SnapContext& snapc,
	       uint64_t offset,
	       uint64_t len,
	       __u32 truncate_seq,
	       utime_t mtime,
	       int flags,
	       Context *onack,
	       Context *oncommit) {
    vector<ObjectExtent> extents;
    file_to_extents(cct, ino, layout, offset, len, extents);
    if (extents.size() == 1) {
      vector<OSDOp> ops(1);
      ops[0].op.op = CEPH_OSD_OP_TRIMTRUNC;
      ops[0].op.extent.truncate_seq = truncate_seq;
      ops[0].op.extent.truncate_size = extents[0].offset;
      objecter->_modify(extents[0].oid, extents[0].oloc, ops, mtime, snapc, flags, onack, oncommit);
    } else {
      C_GatherBuilder gack(cct, onack);
      C_GatherBuilder gcom(cct, oncommit);
      for (vector<ObjectExtent>::iterator p = extents.begin(); p != extents.end(); p++) {
	vector<OSDOp> ops(1);
	ops[0].op.op = CEPH_OSD_OP_TRIMTRUNC;
	ops[0].op.extent.truncate_size = p->offset;
	ops[0].op.extent.truncate_seq = truncate_seq;
	objecter->_modify(p->oid, p->oloc, ops, mtime, snapc, flags,
			  onack ? gack.new_sub():0,
			  oncommit ? gcom.new_sub():0);
      }
      gack.activate();
      gcom.activate();
    }
    return 0;
  }

  int zero(inodeno_t ino,
	   ceph_file_layout *layout,
	   const SnapContext& snapc,
	   uint64_t offset,
           uint64_t len,
	   utime_t mtime,
	   int flags,
           Context *onack,
           Context *oncommit) {
    vector<ObjectExtent> extents;
    file_to_extents(cct, ino, layout, offset, len, extents);
    if (extents.size() == 1) {
      if (extents[0].offset == 0 && extents[0].length == layout->fl_object_size)
	objecter->remove(extents[0].oid, extents[0].oloc, 
			 snapc, mtime, flags, onack, oncommit);
      else
	objecter->zero(extents[0].oid, extents[0].oloc, extents[0].offset, extents[0].length, 
		       snapc, mtime, flags, onack, oncommit);
    } else {
      C_GatherBuilder gack(cct, onack);
      C_GatherBuilder gcom(cct, oncommit);
      for (vector<ObjectExtent>::iterator p = extents.begin(); p != extents.end(); p++) {
	if (p->offset == 0 && p->length == layout->fl_object_size)
	  objecter->remove(p->oid, p->oloc,
			   snapc, mtime, flags,
			   onack ? gack.new_sub():0,
			   oncommit ? gcom.new_sub():0);
	else
	  objecter->zero(p->oid, p->oloc, p->offset, p->length, 
			 snapc, mtime, flags,
			 onack ? gack.new_sub():0,
			 oncommit ? gcom.new_sub():0);
      }
      gack.activate();
      gcom.activate();
    }
    return 0;
  }

  // purge range of ino.### objects
  int purge_range(inodeno_t ino,
		  ceph_file_layout *layout,
		  const SnapContext& snapc,
		  uint64_t first_obj, uint64_t num_obj,
		  utime_t mtime,
		  int flags,
		  Context *oncommit);
  void _do_purge_range(class PurgeRange *pr, int fin);

  /*
   * probe 
   *  specify direction,
   *  and whether we stop when we find data, or hole.
   */
  int probe(inodeno_t ino,
	    ceph_file_layout *layout,
	    snapid_t snapid,
	    uint64_t start_from,
	    uint64_t *end,
	    utime_t *mtime,
	    bool fwd,
	    int flags,
	    Context *onfinish);
};



#endif
