//////////////////////////////////////////////////////////////////////////////
// Copyright (c) 2010-2014, Lawrence Livermore National Security, LLC.
// Produced at the Lawrence Livermore National Laboratory.
//
// This file is part of the Callpath library.
// Written by Todd Gamblin, tgamblin@llnl.gov, All rights reserved.
// LLNL-CODE-647183
//
// For details, see https://github.com/scalability-llnl/callpath
//
// For details, see https://scalability-llnl.github.io/spack
// Please also see the LICENSE file for our notice and the LGPL.
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License (as published by
// the Free Software Foundation) version 2.1 dated February 1999.
//
// This program is distributed in the hope that it will be useful, but
// WITHOUT ANY WARRANTY; without even the IMPLIED WARRANTY OF
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the terms and
// conditions of the GNU General Public License for more details.
//
// You should have received a copy of the GNU Lesser General Public License
// along with this program; if not, write to the Free Software Foundation,
// Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
//////////////////////////////////////////////////////////////////////////////
#include "Callpath.h"

#include <string>
#include <iostream>
#include <fstream>
#include <sstream>
#include <set>
#include <vector>
#include <map>
///the function reverse is prototyped in algorithm on AIX. Not needed for other machine but also
///not harmful
#include <algorithm>
using namespace std;

#ifdef CALLPATH_HAVE_MPI
#include "mpi_utils.h"
#endif //CALLPATH_HAVE_MPI

#include "io_utils.h"
using namespace io_utils;

#include "string_utils.h"
using namespace stringutils;

/// This is the set of all unique callpaths seen so far.  Used to unique
/// callpaths on creation, so that instances can be compared by pointer.
typedef set<const vector<FrameId>*, pathvector_lt<less<FrameId> > > callpath_set;

static callpath_set& paths() {
  static callpath_set pathset;
  return pathset;
}

Callpath::Callpath(const vector<FrameId> *p) : path(p) { }


Callpath::Callpath(const Callpath& other) : path(other.path) { }


Callpath Callpath::create(const vector<FrameId>& path) {
  callpath_set::iterator u = paths().find(&path);
  if (u == paths().end()) {
    // if the vector isn't in there already then create a copy to add
    vector<FrameId> *temp = new vector<FrameId>(path);
    u = paths().insert(temp).first;
  }
  return Callpath(*u);
}


Callpath& Callpath::operator=(const Callpath& other) {
  path = other.path;
  return *this;
}


#ifdef CALLPATH_HAVE_MPI

size_t Callpath::packed_size(MPI_Comm comm) const {
  size_t pack_size = 0;
  pack_size += pmpi_packed_size(1, MPI_INT, comm);  // number of frames
  for (size_t i=0; i < size(); i++) {              // size of each frame
    pack_size += (*path)[i].packed_size(comm);
  }
  return pack_size;
}


void Callpath::pack(void *buf, int bufsize, int *position, MPI_Comm comm) const {
  int len = size();
  PMPI_Pack(&len, 1, MPI_INT, buf, bufsize, position, comm);
  for (int i=0; i < len; i++) {
    (*path)[i].pack(buf, bufsize, position, comm);
  }
}


Callpath Callpath::unpack(const ModuleId::id_map& modules, void *buf, int bufsize, int *position, MPI_Comm comm) {
  int len;
  PMPI_Unpack(buf, bufsize, position, &len, 1, MPI_INT, comm); // number of elements

  if (!len) return Callpath();

  vector<FrameId> path;
  for (int i=0; i < len; i++) {
    // unpack and translate module address, then push onto the vector.
    path.push_back(FrameId::unpack(modules, buf, bufsize, position, comm));
  }
  return create(path);
}
#endif // CALLPATH_HAVE_MPI


std::ostream& operator<<(std::ostream& out, const Callpath& cp) {
  if (!cp.path) {
    out << "null_callpath";

  } else {
    vector<FrameId>::const_reverse_iterator i=cp.path->rbegin();
    if (i != cp.path->rend()) {
      out << i->module << "(0x" << hex << i->offset << ")";
      i++;
    }
    while (i != cp.path->rend()) {
      out << " : "
          << i->module << "(0x" << hex << i->offset << ")";
      i++;
    }
  }
  out << dec; // revert to decimal.
  return out;
}


size_t Callpath::size() const {
  return path ? path->size() : 0;
}


bool Callpath::in(const Callpath& other) const {
  if (other.size() > size()) {
    return false;
  } else {
    return equal(other.path->rbegin(), other.path->rend(), path->rbegin());
  }
}


Callpath Callpath::slice(size_t start, size_t end) {
  vector<FrameId> new_slice;
  for (size_t i=start; i < end; i++) {
    new_slice.push_back((*path)[i]);
  }
  return create(new_slice);
}

Callpath Callpath::slice(size_t start) {
  return slice(start, size());
}


void Callpath::write_out(ostream& out) {
  // build set of modules referenced in this particular callpath
  set<ModuleId> my_modules;
  for (size_t i=0; i < size(); i++) {
    my_modules.insert((*path)[i].module);
  }

  // write out names/string addrs of all module strings used here
  vl_write(out, my_modules.size());
  for (set<ModuleId>::iterator i=my_modules.begin(); i != my_modules.end(); i++) {
    i->write_id(out);
    i->write_out(out);
  }

  // now write out each raw frame id
  vl_write(out, size());
  for (size_t i=0; i < size(); i++) {
    (*path)[i].write_out(out);
  }
}


Callpath Callpath::read_in(istream& in) {
  size_t num_modules = vl_read(in);

  // read in strings and make entries to translate addresses
  ModuleId::id_map trans;
  for (size_t i=0; i < num_modules; i++) {
    uintptr_t addr = vl_read(in);
    trans.insert(ModuleId::id_map::value_type(addr, ModuleId::read_in(in)));
  }

  size_t len = vl_read(in);
  if (!len) return Callpath(NULL);

  vector<FrameId> temp;
  for (size_t i=0; i < len; i++) {
    temp.push_back(FrameId::read_in(trans, in));
  }
  return create(temp);
}

void Callpath::dump(ostream& out) {
  out << paths().size() << " total paths" << endl;

  for (callpath_set::iterator i=paths().begin(); i != paths().end(); i++) {
    out << Callpath(*i) << endl;
  }
}


const FrameId& Callpath::get(size_t i) const {
  if (i > size()) {
    cerr << "Index out of bounds: " << i << endl;
    exit(1);
  }
  return (*path)[i];
}


Callpath make_path(const string& path) {
  vector<string> frame_strings;
  vector<FrameId> frames;

  split(path, ":", frame_strings);
  for (size_t i=0; i < frame_strings.size(); i++) {
    vector<string> pieces;
    split(frame_strings[i], "(", pieces);

    string module_str, offset_str;
    if (pieces.size() == 1) {
      offset_str = trim(pieces[0], "( )");
    } else if (pieces.size() == 2) {
      module_str = trim(pieces[0], "( )");
      offset_str = trim(pieces[1], "( )");
    } else {
      cerr << "ERROR: bad callpath parse splitting '" << frame_strings[i] << "'" << endl;
      exit(1);
    }

    ModuleId module(module_str);
    char *err;
    uintptr_t offset = strtoull(offset_str.c_str(), &err, 0);
    if (*err) {
      cerr << "ERROR: bad callpath parse at: '" << hex << offset << dec << "'" << endl;
      exit(1);
    }

    frames.push_back(FrameId(module, offset));
  }
  reverse(frames.begin(), frames.end());

  return Callpath::create(frames);
}
