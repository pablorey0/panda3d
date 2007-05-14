// Filename: geomVertexArrayData.cxx
// Created by:  drose (17Mar05)
//
////////////////////////////////////////////////////////////////////
//
// PANDA 3D SOFTWARE
// Copyright (c) 2001 - 2004, Disney Enterprises, Inc.  All rights reserved
//
// All use of this software is subject to the terms of the Panda 3d
// Software license.  You should have received a copy of this license
// along with this source code; you will also find a current copy of
// the license at http://etc.cmu.edu/panda3d/docs/license/ .
//
// To contact the maintainers of this program write to
// panda3d-general@lists.sourceforge.net .
//
////////////////////////////////////////////////////////////////////

#include "geomVertexArrayData.h"
#include "geom.h"
#include "preparedGraphicsObjects.h"
#include "reversedNumericData.h"
#include "bamReader.h"
#include "bamWriter.h"
#include "pset.h"
#include "config_gobj.h"
#include "pStatTimer.h"
#include "configVariableInt.h"
#include "vertexDataSaveFile.h"
#include "simpleAllocator.h"

#ifdef HAVE_ZLIB
#include <zlib.h>
#endif

ConfigVariableInt max_ram_vertex_data
("max-ram-vertex-data", -1,
 PRC_DESC("Specifies the maximum number of bytes of all vertex data "
          "that is allowed to remain resident in system RAM at one time. "
          "If more than this number of bytes of vertices are created, "
          "the least-recently-used ones will be temporarily compressed in "
          "system RAM until they are needed.  Set it to -1 for no limit."));

ConfigVariableInt max_compressed_vertex_data
("max-compressed-vertex-data", -1,
 PRC_DESC("Specifies the maximum number of bytes of all vertex data "
          "that is allowed to remain compressed in system RAM at one time. "
          "If more than this number of bytes of vertices are created, "
          "the least-recently-used ones will be temporarily flushed to "
          "disk until they are needed.  Set it to -1 for no limit."));

ConfigVariableInt vertex_data_compression_level
("vertex-data-compression-level", 1,
 PRC_DESC("Specifies the zlib compression level to use when compressing "
          "vertex data.  The number should be in the range 1 to 9, where "
          "larger values are slower but give better compression."));

ConfigVariableInt max_disk_vertex_data
("max-disk-vertex-data", -1,
 PRC_DESC("Specifies the maximum number of bytes of vertex data "
          "that is allowed to be written to disk.  Set it to -1 for no "
          "limit."));

// We make this a static constant rather than a dynamic variable,
// since we can't tolerate this value changing at runtime.
static const size_t min_vertex_data_compress_size = 
  ConfigVariableInt
  ("min-vertex-data-compress-size", 64,
   PRC_DESC("This is the minimum number of bytes that we deem worthy of "
            "passing through zlib to compress, when a vertex buffer is "
            "evicted from resident state and compressed for long-term "
            "storage.  Buffers smaller than this are assumed to be likely to "
            "have minimal compression gains (or even end up larger)."));

SimpleLru GeomVertexArrayData::_ram_lru(max_ram_vertex_data);
SimpleLru GeomVertexArrayData::_compressed_lru(max_compressed_vertex_data);
SimpleLru GeomVertexArrayData::_disk_lru(0);

SimpleLru *GeomVertexArrayData::_global_lru[RC_end_of_list] = {
  &GeomVertexArrayData::_ram_lru,
  &GeomVertexArrayData::_compressed_lru,
  &GeomVertexArrayData::_disk_lru,
  &GeomVertexArrayData::_disk_lru,
};

VertexDataSaveFile *GeomVertexArrayData::_save_file;

PStatCollector GeomVertexArrayData::_vdata_compress_pcollector("*:Vertex Data:Compress");
PStatCollector GeomVertexArrayData::_vdata_decompress_pcollector("*:Vertex Data:Decompress");
PStatCollector GeomVertexArrayData::_vdata_save_pcollector("*:Vertex Data:Save");
PStatCollector GeomVertexArrayData::_vdata_restore_pcollector("*:Vertex Data:Restore");


TypeHandle GeomVertexArrayData::_type_handle;
TypeHandle GeomVertexArrayData::CData::_type_handle;
TypeHandle GeomVertexArrayDataHandle::_type_handle;

////////////////////////////////////////////////////////////////////
//     Function: GeomVertexArrayData::Default Constructor
//       Access: Private
//  Description: Constructs an invalid object.  This is only used when
//               reading from the bam file.
////////////////////////////////////////////////////////////////////
GeomVertexArrayData::
GeomVertexArrayData() : SimpleLruPage(0) {
  _endian_reversed = false;
  _ram_class = RC_resident;
  _saved_block = NULL;

  // Can't put it in the LRU until it has been read in and made valid.
}

////////////////////////////////////////////////////////////////////
//     Function: GeomVertexArrayData::make_cow_copy
//       Access: Protected, Virtual
//  Description: Required to implement CopyOnWriteObject.
////////////////////////////////////////////////////////////////////
PT(CopyOnWriteObject) GeomVertexArrayData::
make_cow_copy() {
  make_resident();
  return new GeomVertexArrayData(*this);
}

////////////////////////////////////////////////////////////////////
//     Function: GeomVertexArrayData::Constructor
//       Access: Published
//  Description: 
////////////////////////////////////////////////////////////////////
GeomVertexArrayData::
GeomVertexArrayData(const GeomVertexArrayFormat *array_format,
                    GeomVertexArrayData::UsageHint usage_hint) :
  SimpleLruPage(0),
  _array_format(array_format)
{
  OPEN_ITERATE_ALL_STAGES(_cycler) {
    CDStageWriter cdata(_cycler, pipeline_stage);
    cdata->_usage_hint = usage_hint;
  }
  CLOSE_ITERATE_ALL_STAGES(_cycler);

  _endian_reversed = false;

  _ram_class = RC_resident;
  _saved_block = NULL;
  mark_used_lru(_global_lru[RC_resident]);

  nassertv(_array_format->is_registered());
}
  
////////////////////////////////////////////////////////////////////
//     Function: GeomVertexArrayData::Copy Constructor
//       Access: Published
//  Description: 
////////////////////////////////////////////////////////////////////
GeomVertexArrayData::
GeomVertexArrayData(const GeomVertexArrayData &copy) :
  CopyOnWriteObject(copy),
  SimpleLruPage(copy),
  _array_format(copy._array_format),
  _cycler(copy._cycler)
{
  _endian_reversed = false;

  _ram_class = copy._ram_class;
  _saved_block = NULL;
  mark_used_lru(_global_lru[_ram_class]);

  nassertv(_array_format->is_registered());
}

////////////////////////////////////////////////////////////////////
//     Function: GeomVertexArrayData::Copy Assignment Operator
//       Access: Published
//  Description: The copy assignment operator is not pipeline-safe.
//               This will completely obliterate all stages of the
//               pipeline, so don't do it for a GeomVertexArrayData
//               that is actively being used for rendering.
////////////////////////////////////////////////////////////////////
void GeomVertexArrayData::
operator = (const GeomVertexArrayData &copy) {
  CopyOnWriteObject::operator = (copy);
  SimpleLruPage::operator = (copy);
  _array_format = copy._array_format;
  _cycler = copy._cycler;

  OPEN_ITERATE_ALL_STAGES(_cycler) {
    CDStageWriter cdata(_cycler, pipeline_stage);
    cdata->_modified = Geom::get_next_modified();
  }
  CLOSE_ITERATE_ALL_STAGES(_cycler);

  nassertv(_array_format->is_registered());
}

////////////////////////////////////////////////////////////////////
//     Function: GeomVertexArrayData::Destructor
//       Access: Published, Virtual
//  Description: 
////////////////////////////////////////////////////////////////////
GeomVertexArrayData::
~GeomVertexArrayData() {
  release_all();

  if (_saved_block != (SimpleAllocatorBlock *)NULL) {
    delete _saved_block;
  }
}

////////////////////////////////////////////////////////////////////
//     Function: GeomVertexArrayData::set_usage_hint
//       Access: Published
//  Description: Changes the UsageHint hint for this array.  See
//               get_usage_hint().
//
//               Don't call this in a downstream thread unless you
//               don't mind it blowing away other changes you might
//               have recently made in an upstream thread.
////////////////////////////////////////////////////////////////////
void GeomVertexArrayData::
set_usage_hint(GeomVertexArrayData::UsageHint usage_hint) {
  CDWriter cdata(_cycler, true);
  cdata->_usage_hint = usage_hint;
  cdata->_modified = Geom::get_next_modified();
}

////////////////////////////////////////////////////////////////////
//     Function: GeomVertexArrayData::output
//       Access: Published
//  Description: 
////////////////////////////////////////////////////////////////////
void GeomVertexArrayData::
output(ostream &out) const {
  out << get_num_rows() << " rows: " << *get_array_format();
}

////////////////////////////////////////////////////////////////////
//     Function: GeomVertexArrayData::write
//       Access: Published
//  Description: 
////////////////////////////////////////////////////////////////////
void GeomVertexArrayData::
write(ostream &out, int indent_level) const {
  _array_format->write_with_data(out, indent_level, this);
}

////////////////////////////////////////////////////////////////////
//     Function: GeomVertexArrayData::prepare
//       Access: Public
//  Description: Indicates that the data should be enqueued to be
//               prepared in the indicated prepared_objects at the
//               beginning of the next frame.  This will ensure the
//               data is already loaded into the GSG if it is expected
//               to be rendered soon.
//
//               Use this function instead of prepare_now() to preload
//               datas from a user interface standpoint.
////////////////////////////////////////////////////////////////////
void GeomVertexArrayData::
prepare(PreparedGraphicsObjects *prepared_objects) {
  prepared_objects->enqueue_vertex_buffer(this);
}

////////////////////////////////////////////////////////////////////
//     Function: GeomVertexArrayData::is_prepared
//       Access: Published
//  Description: Returns true if the data has already been prepared
//               or enqueued for preparation on the indicated GSG,
//               false otherwise.
////////////////////////////////////////////////////////////////////
bool GeomVertexArrayData::
is_prepared(PreparedGraphicsObjects *prepared_objects) const {
  Contexts::const_iterator ci;
  ci = _contexts.find(prepared_objects);
  if (ci != _contexts.end()) {
    return true;
  }
  return prepared_objects->is_vertex_buffer_queued(this);
}

////////////////////////////////////////////////////////////////////
//     Function: GeomVertexArrayData::prepare_now
//       Access: Public
//  Description: Creates a context for the data on the particular
//               GSG, if it does not already exist.  Returns the new
//               (or old) VertexBufferContext.  This assumes that the
//               GraphicsStateGuardian is the currently active
//               rendering context and that it is ready to accept new
//               datas.  If this is not necessarily the case, you
//               should use prepare() instead.
//
//               Normally, this is not called directly except by the
//               GraphicsStateGuardian; a data does not need to be
//               explicitly prepared by the user before it may be
//               rendered.
////////////////////////////////////////////////////////////////////
VertexBufferContext *GeomVertexArrayData::
prepare_now(PreparedGraphicsObjects *prepared_objects, 
            GraphicsStateGuardianBase *gsg) {
  Contexts::const_iterator ci;
  ci = _contexts.find(prepared_objects);
  if (ci != _contexts.end()) {
    return (*ci).second;
  }

  VertexBufferContext *vbc = prepared_objects->prepare_vertex_buffer_now(this, gsg);
  if (vbc != (VertexBufferContext *)NULL) {
    _contexts[prepared_objects] = vbc;
  }
  return vbc;
}

////////////////////////////////////////////////////////////////////
//     Function: GeomVertexArrayData::release
//       Access: Public
//  Description: Frees the data context only on the indicated object,
//               if it exists there.  Returns true if it was released,
//               false if it had not been prepared.
////////////////////////////////////////////////////////////////////
bool GeomVertexArrayData::
release(PreparedGraphicsObjects *prepared_objects) {
  Contexts::iterator ci;
  ci = _contexts.find(prepared_objects);
  if (ci != _contexts.end()) {
    VertexBufferContext *vbc = (*ci).second;
    prepared_objects->release_vertex_buffer(vbc);
    return true;
  }

  // Maybe it wasn't prepared yet, but it's about to be.
  return prepared_objects->dequeue_vertex_buffer(this);
}

////////////////////////////////////////////////////////////////////
//     Function: GeomVertexArrayData::release_all
//       Access: Public
//  Description: Frees the context allocated on all objects for which
//               the data has been declared.  Returns the number of
//               contexts which have been freed.
////////////////////////////////////////////////////////////////////
int GeomVertexArrayData::
release_all() {
  // We have to traverse a copy of the _contexts list, because the
  // PreparedGraphicsObjects object will call clear_prepared() in response
  // to each release_vertex_buffer(), and we don't want to be modifying the
  // _contexts list while we're traversing it.
  Contexts temp = _contexts;
  int num_freed = (int)_contexts.size();

  Contexts::const_iterator ci;
  for (ci = temp.begin(); ci != temp.end(); ++ci) {
    PreparedGraphicsObjects *prepared_objects = (*ci).first;
    VertexBufferContext *vbc = (*ci).second;
    prepared_objects->release_vertex_buffer(vbc);
  }

  // Now that we've called release_vertex_buffer() on every known context,
  // the _contexts list should have completely emptied itself.
  nassertr(_contexts.empty(), num_freed);

  return num_freed;
}

////////////////////////////////////////////////////////////////////
//     Function: GeomVertexArrayData::lru_epoch
//       Access: Published, Static
//  Description: Marks that an epoch has passed in each LRU.  Asks the
//               LRU's to consider whether they should perform
//               evictions.
////////////////////////////////////////////////////////////////////
void GeomVertexArrayData::
lru_epoch() {
  _ram_lru.begin_epoch();
  _compressed_lru.begin_epoch();

  // No automatic eviction from the Disk LRU.
  //_disk_lru.begin_epoch();
}

////////////////////////////////////////////////////////////////////
//     Function: GeomVertexArrayData::make_resident
//       Access: Published
//  Description: Moves the vertex data to fully resident status by
//               expanding it or reading it from disk as necessary.
////////////////////////////////////////////////////////////////////
void GeomVertexArrayData::
make_resident() {
  // TODO: make this work with pipelining properly.

  if (_ram_class == RC_resident) {
    // If we're already resident, just mark the page recently used.
    mark_used_lru();
    return;
  }

  if (_ram_class == RC_disk || _ram_class == RC_compressed_disk) {
    restore_from_disk();
  }

  if (_ram_class == RC_compressed) {
    CDWriter cdata(_cycler, true);
#ifdef HAVE_ZLIB
    if (cdata->_data_full_size > min_vertex_data_compress_size) {
      PStatTimer timer(_vdata_decompress_pcollector);

      if (gobj_cat.is_debug()) {
        gobj_cat.debug()
          << "Expanding " << *this << " from " << cdata->_data.size()
          << " to " << cdata->_data_full_size << "\n";
      }
      Data new_data(cdata->_data_full_size, get_class_type());
      uLongf dest_len = cdata->_data_full_size;
      int result = uncompress(&new_data[0], &dest_len,
                              &cdata->_data[0], cdata->_data.size());
      if (result != Z_OK) {
        gobj_cat.error()
          << "Couldn't expand: zlib error " << result << "\n";
        nassert_raise("zlib error");
      }
      nassertv(dest_len == new_data.size());
      cdata->_data.swap(new_data);
    }
#endif
    set_lru_size(cdata->_data.size());
    set_ram_class(RC_resident);
  }
}

////////////////////////////////////////////////////////////////////
//     Function: GeomVertexArrayData::make_compressed
//       Access: Published
//  Description: Moves the vertex data to compressed status by
//               compressing it or reading it from disk as necessary.
////////////////////////////////////////////////////////////////////
void GeomVertexArrayData::
make_compressed() {
  // TODO: make this work with pipelining properly.

  if (_ram_class == RC_compressed) {
    // If we're already compressed, just mark the page recently used.
    mark_used_lru();
    return;
  }

  if (_ram_class == RC_disk || _ram_class == RC_compressed_disk) {
    restore_from_disk();
  }

  if (_ram_class == RC_resident) {
    CDWriter cdata(_cycler, true);
#ifdef HAVE_ZLIB
    if (cdata->_data_full_size > min_vertex_data_compress_size) {
      PStatTimer timer(_vdata_compress_pcollector);

      // According to the zlib manual, we need to provide this much
      // buffer to the compress algorithm: 0.1% bigger plus twelve
      // bytes.
      uLongf buffer_size = cdata->_data_full_size + ((cdata->_data_full_size + 999) / 1000) + 12;
      Bytef *buffer = new Bytef[buffer_size];

      int result = compress2(buffer, &buffer_size,
                             &cdata->_data[0], cdata->_data_full_size,
                             vertex_data_compression_level);
      if (result != Z_OK) {
        gobj_cat.error()
          << "Couldn't compress: zlib error " << result << "\n";
        nassert_raise("zlib error");
      }
    
      Data new_data(buffer_size, get_class_type());
      memcpy(&new_data[0], buffer, buffer_size);
      delete[] buffer;

      cdata->_data.swap(new_data);
      if (gobj_cat.is_debug()) {
        gobj_cat.debug()
          << "Compressed " << *this << " from " << cdata->_data_full_size
          << " to " << cdata->_data.size() << "\n";
      }
    }
#endif
    set_lru_size(cdata->_data.size());
    set_ram_class(RC_compressed);
  }
}

////////////////////////////////////////////////////////////////////
//     Function: GeomVertexArrayData::make_disk
//       Access: Published
//  Description: Moves the vertex data to disk status by
//               writing it to disk as necessary.
////////////////////////////////////////////////////////////////////
void GeomVertexArrayData::
make_disk() {
  // TODO: make this work with pipelining properly.

  if (_ram_class == RC_disk || _ram_class == RC_compressed_disk) {
    // If we're already compressed, just mark the page recently used.
    mark_used_lru();
    return;
  }

  if (_ram_class == RC_resident || _ram_class == RC_compressed) {
    nassertv(_saved_block == (SimpleAllocatorBlock *)NULL);
    CDWriter cdata(_cycler, true);

    PStatTimer timer(_vdata_save_pcollector);

    if (gobj_cat.is_debug()) {
      gobj_cat.debug()
        << "Storing " << *this << ", " << cdata->_data.size()
        << " bytes, to disk\n";
    }

    const unsigned char *data = &cdata->_data[0];
    size_t size = cdata->_data.size();
    _saved_block = get_save_file()->write_data(data, size);
    if (_saved_block == NULL) {
      // Can't write it to disk.  Too bad.
      mark_used_lru();
      return;
    }

    // We swap the pvector with an empty one, to really make it free
    // its memory.  Otherwise it might optimistically keep the memory
    // allocated.
    Data new_data(get_class_type());
    cdata->_data.swap(new_data);

    if (_ram_class == RC_resident) {
      set_ram_class(RC_disk);
    } else {
      set_ram_class(RC_compressed_disk);
    }
  }
}

////////////////////////////////////////////////////////////////////
//     Function: GeomVertexArrayData::restore_from_disk
//       Access: Published
//  Description: Restores the vertex data from disk and makes it
//               either compressed or resident (according to whether
//               it was stored compressed on disk).
////////////////////////////////////////////////////////////////////
void GeomVertexArrayData::
restore_from_disk() {
  if (_ram_class == RC_disk || _ram_class == RC_compressed_disk) {
    nassertv(_saved_block != (SimpleAllocatorBlock *)NULL);
    CDWriter cdata(_cycler, true);

    PStatTimer timer(_vdata_restore_pcollector);

    size_t size = _saved_block->get_size();
    if (gobj_cat.is_debug()) {
      gobj_cat.debug()
        << "Restoring " << *this << ", " << size
        << " bytes, from disk\n";
    }

    Data new_data(size, get_class_type());
    if (!get_save_file()->read_data(&new_data[0], size, _saved_block)) {
      nassert_raise("read error");
    }
    cdata->_data.swap(new_data);

    delete _saved_block;
    _saved_block = NULL;

    if (_ram_class == RC_compressed_disk) {
      set_ram_class(RC_compressed);
    } else {
      set_ram_class(RC_resident);
    }
  }
}

////////////////////////////////////////////////////////////////////
//     Function: GeomVertexArrayData::evict_lru
//       Access: Public, Virtual
//  Description: Evicts the page from the LRU.  Called internally when
//               the LRU determines that it is full.  May also be
//               called externally when necessary to explicitly evict
//               the page.
//
//               It is legal for this method to either evict the page
//               as requested, do nothing (in which case the eviction
//               will be requested again at the next epoch), or
//               requeue itself on the tail of the queue (in which
//               case the eviction will be requested again much
//               later).
////////////////////////////////////////////////////////////////////
void GeomVertexArrayData::
evict_lru() {
  nassertv(get_lru() == get_global_lru(_ram_class));

  switch (_ram_class) {
  case RC_resident:
    if (_compressed_lru.get_max_size() == 0) {
      make_disk();
    } else {
      make_compressed();
    }
    break;

  case RC_compressed:
    make_disk();
    break;

  case RC_disk:
  case RC_compressed_disk:
    gobj_cat.warning()
      << "Cannot evict array data from disk.\n";
    break;

  case RC_end_of_list:
    break;
  }
}

////////////////////////////////////////////////////////////////////
//     Function: GeomVertexArrayData::clear_prepared
//       Access: Private
//  Description: Removes the indicated PreparedGraphicsObjects table
//               from the data array's table, without actually
//               releasing the data array.  This is intended to be
//               called only from
//               PreparedGraphicsObjects::release_vertex_buffer(); it should
//               never be called by user code.
////////////////////////////////////////////////////////////////////
void GeomVertexArrayData::
clear_prepared(PreparedGraphicsObjects *prepared_objects) {
  Contexts::iterator ci;
  ci = _contexts.find(prepared_objects);
  if (ci != _contexts.end()) {
    _contexts.erase(ci);
  } else {
    // If this assertion fails, clear_prepared() was given a
    // prepared_objects which the data array didn't know about.
    nassertv(false);
  }
}

////////////////////////////////////////////////////////////////////
//     Function: GeomVertexArrayData::reverse_data_endianness
//       Access: Private
//  Description: Fills a new data array with all numeric values
//               expressed in the indicated array reversed,
//               byte-for-byte, to convert littleendian to bigendian
//               and vice-versa.
////////////////////////////////////////////////////////////////////
void GeomVertexArrayData::
reverse_data_endianness(unsigned char *dest, const unsigned char *source, 
                        size_t size) {
  int num_columns = _array_format->get_num_columns();

  // Walk through each row of the data.
  for (size_t pi = 0; pi < size; pi += _array_format->get_stride()) {
    // For each row, visit all of the columns; and for each column,
    // visit all of the components of that column.
    for (int ci = 0; ci < num_columns; ++ci) {
      const GeomVertexColumn *col = _array_format->get_column(ci);
      int component_bytes = col->get_component_bytes();
      if (component_bytes > 1) {
        // Get the index of the beginning of the column.
        size_t ci = pi + col->get_start();

        int num_components = col->get_num_components();
        for (int cj = 0; cj < num_components; ++cj) {
          // Reverse the bytes of each component.
          ReversedNumericData nd(source + ci, component_bytes);
          nd.store_value(dest + ci, component_bytes);
          pi += component_bytes;
        }
      }
    }
  }
}

////////////////////////////////////////////////////////////////////
//     Function: GeomVertexArrayData::make_save_file
//       Access: Private, Static
//  Description: Creates the global VertexDataSaveFile that will be
//               used to save vertex data buffers to disk when
//               necessary.
////////////////////////////////////////////////////////////////////
void GeomVertexArrayData::
make_save_file() {
  size_t max_size = (size_t)max_disk_vertex_data;

  _save_file = new VertexDataSaveFile(vertex_save_file_directory,
                                      vertex_save_file_prefix, max_size);
}

////////////////////////////////////////////////////////////////////
//     Function: GeomVertexArrayData::register_with_read_factory
//       Access: Public, Static
//  Description: Tells the BamReader how to create objects of type
//               GeomVertexArrayData.
////////////////////////////////////////////////////////////////////
void GeomVertexArrayData::
register_with_read_factory() {
  BamReader::get_factory()->register_factory(get_class_type(), make_from_bam);
}

////////////////////////////////////////////////////////////////////
//     Function: GeomVertexArrayData::write_datagram
//       Access: Public, Virtual
//  Description: Writes the contents of this object to the datagram
//               for shipping out to a Bam file.
////////////////////////////////////////////////////////////////////
void GeomVertexArrayData::
write_datagram(BamWriter *manager, Datagram &dg) {
  make_resident();
  CopyOnWriteObject::write_datagram(manager, dg);

  manager->write_pointer(dg, _array_format);
  manager->write_cdata(dg, _cycler, this);
}

////////////////////////////////////////////////////////////////////
//     Function: GeomVertexArrayData::read_raw_data
//       Access: Public
//  Description: Called by CData::fillin to read the raw data
//               of the array from the indicated datagram.
////////////////////////////////////////////////////////////////////
PTA_uchar GeomVertexArrayData::
read_raw_data(BamReader *manager, DatagramIterator &scan) {
  size_t size = scan.get_uint32();
  PTA_uchar data = PTA_uchar::empty_array(size, get_class_type());
  const unsigned char *source_data = 
    (const unsigned char *)scan.get_datagram().get_data();
  memcpy(data, source_data + scan.get_current_index(), size);
  scan.skip_bytes(size);

  return data;
}

////////////////////////////////////////////////////////////////////
//     Function: GeomVertexArrayData::complete_pointers
//       Access: Public, Virtual
//  Description: Receives an array of pointers, one for each time
//               manager->read_pointer() was called in fillin().
//               Returns the number of pointers processed.
////////////////////////////////////////////////////////////////////
int GeomVertexArrayData::
complete_pointers(TypedWritable **p_list, BamReader *manager) {
  int pi = CopyOnWriteObject::complete_pointers(p_list, manager);

  _array_format = DCAST(GeomVertexArrayFormat, p_list[pi++]);

  return pi;
}

////////////////////////////////////////////////////////////////////
//     Function: GeomVertexArrayData::finalize
//       Access: Public, Virtual
//  Description: Called by the BamReader to perform any final actions
//               needed for setting up the object after all objects
//               have been read and all pointers have been completed.
////////////////////////////////////////////////////////////////////
void GeomVertexArrayData::
finalize(BamReader *manager) {
  // Now we need to register the format that we have read from the bam
  // file (since it doesn't come out of the bam file automatically
  // registered).  This may change the format's pointer, which we
  // should then update our own data to reflect.  But since this may
  // cause the unregistered object to destruct, we have to also tell
  // the BamReader to return the new object from now on.

  CDWriter cdata(_cycler, true);

  CPT(GeomVertexArrayFormat) new_array_format = 
    GeomVertexArrayFormat::register_format(_array_format);

  manager->change_pointer(_array_format, new_array_format);
  _array_format = new_array_format;

  if (_endian_reversed) {
    // Now is the time to endian-reverse the data.
    Data new_data(cdata->_data.size(), get_class_type());
    reverse_data_endianness(&new_data[0], &cdata->_data[0], cdata->_data.size());
    cdata->_data.swap(new_data);
  }

  set_ram_class(RC_resident);
}

////////////////////////////////////////////////////////////////////
//     Function: GeomVertexArrayData::make_from_bam
//       Access: Protected, Static
//  Description: This function is called by the BamReader's factory
//               when a new object of type GeomVertexArrayData is encountered
//               in the Bam file.  It should create the GeomVertexArrayData
//               and extract its information from the file.
////////////////////////////////////////////////////////////////////
TypedWritable *GeomVertexArrayData::
make_from_bam(const FactoryParams &params) {
  GeomVertexArrayData *object = new GeomVertexArrayData;
  DatagramIterator scan;
  BamReader *manager;

  parse_params(params, scan, manager);
  object->fillin(scan, manager);
  manager->register_finalize(object);

  return object;
}

////////////////////////////////////////////////////////////////////
//     Function: GeomVertexArrayData::fillin
//       Access: Protected
//  Description: This internal function is called by make_from_bam to
//               read in all of the relevant data from the BamFile for
//               the new GeomVertexArrayData.
////////////////////////////////////////////////////////////////////
void GeomVertexArrayData::
fillin(DatagramIterator &scan, BamReader *manager) {
  CopyOnWriteObject::fillin(scan, manager);

  manager->read_pointer(scan);
  manager->read_cdata(scan, _cycler, this);
}

////////////////////////////////////////////////////////////////////
//     Function: GeomVertexArrayData::CData::Destructor
//       Access: Public, Virtual
//  Description:
////////////////////////////////////////////////////////////////////
GeomVertexArrayData::CData::
~CData() {
}

////////////////////////////////////////////////////////////////////
//     Function: GeomVertexArrayData::CData::make_copy
//       Access: Public, Virtual
//  Description:
////////////////////////////////////////////////////////////////////
CycleData *GeomVertexArrayData::CData::
make_copy() const {
  return new CData(*this);
}

////////////////////////////////////////////////////////////////////
//     Function: GeomVertexArrayData::CData::write_datagram
//       Access: Public, Virtual
//  Description: Writes the contents of this object to the datagram
//               for shipping out to a Bam file.
////////////////////////////////////////////////////////////////////
void GeomVertexArrayData::CData::
write_datagram(BamWriter *manager, Datagram &dg, void *extra_data) const {
  GeomVertexArrayData *array_data = (GeomVertexArrayData *)extra_data;
  dg.add_uint8(_usage_hint);

  dg.add_uint32(_data.size());

  if (manager->get_file_endian() == BE_native) {
    // For native endianness, we only have to write the data directly.
    dg.append_data(&_data[0], _data.size());

  } else {
    // Otherwise, we have to convert it.
    Data new_data(_data.size(), GeomVertexArrayData::get_class_type());
    array_data->reverse_data_endianness(&new_data[0], &_data[0], _data.size());
    dg.append_data(&new_data[0], new_data.size());
  }
}

////////////////////////////////////////////////////////////////////
//     Function: GeomVertexArrayData::CData::fillin
//       Access: Public, Virtual
//  Description: This internal function is called by make_from_bam to
//               read in all of the relevant data from the BamFile for
//               the new GeomVertexArrayData.
////////////////////////////////////////////////////////////////////
void GeomVertexArrayData::CData::
fillin(DatagramIterator &scan, BamReader *manager, void *extra_data) {
  GeomVertexArrayData *array_data = (GeomVertexArrayData *)extra_data;
  _usage_hint = (UsageHint)scan.get_uint8();

  if (manager->get_file_minor_ver() < 8) {
    // Before bam version 6.8, the array data was a PTA_uchar.
    PTA_uchar new_data;
    READ_PTA(manager, scan, array_data->read_raw_data, new_data);
    _data = new_data.v();

  } else {
    // Now, the array data is just stored directly.
    size_t size = scan.get_uint32();
    Data new_data(size, GeomVertexArrayData::get_class_type());
    const unsigned char *source_data = 
      (const unsigned char *)scan.get_datagram().get_data();
    memcpy(&new_data[0], source_data + scan.get_current_index(), size);
    scan.skip_bytes(size);

    _data.swap(new_data);
  }

  if (manager->get_file_endian() != BE_native) {
    // For non-native endian files, we have to convert the data.  
    if (array_data->_array_format == (GeomVertexArrayFormat *)NULL) {
      // But we can't do that until we've completed the _array_format
      // pointer, which tells us how to convert it.
      array_data->_endian_reversed = true;
    } else {
      // Since we have the _array_format pointer now, we can reverse
      // it immediately (and we should, to support threaded CData
      // updates).
      Data new_data(_data.size(), GeomVertexArrayData::get_class_type());
      array_data->reverse_data_endianness(&new_data[0], &_data[0], _data.size());
      _data.swap(new_data);
    }
  }

  _data_full_size = _data.size();
  array_data->set_lru_size(_data_full_size);

  _modified = Geom::get_next_modified();
}

////////////////////////////////////////////////////////////////////
//     Function: GeomVertexArrayDataHandle::set_num_rows
//       Access: Public
//  Description: 
////////////////////////////////////////////////////////////////////
bool GeomVertexArrayDataHandle::
set_num_rows(int n) {
  nassertr(_writable, false);
  check_resident();

  int stride = _object->_array_format->get_stride();
  int delta = n - (_cdata->_data.size() / stride);
  
  if (delta != 0) {
    if (delta > 0) {
      _cdata->_data.insert(_cdata->_data.end(), delta * stride, 0);
      
    } else {
      _cdata->_data.erase(_cdata->_data.begin() + n * stride, 
                          _cdata->_data.end());
    }

    _cdata->_modified = Geom::get_next_modified();
    _cdata->_data_full_size = _cdata->_data.size();

    if (get_current_thread()->get_pipeline_stage() == 0) {
      _object->set_ram_class(RC_resident);
      _object->set_lru_size(_cdata->_data_full_size);
    }
    return true;
  }
  
  return false;
}

////////////////////////////////////////////////////////////////////
//     Function: GeomVertexArrayDataHandle::unclean_set_num_rows
//       Access: Public
//  Description: 
////////////////////////////////////////////////////////////////////
bool GeomVertexArrayDataHandle::
unclean_set_num_rows(int n) {
  nassertr(_writable, false);
  check_resident();

  int stride = _object->_array_format->get_stride();
  int delta = n - (_cdata->_data.size() / stride);
  
  if (delta != 0) {
    // Just make a new array.  No reason to keep the old one around.
    GeomVertexArrayData::Data new_data(n * stride, GeomVertexArrayData::get_class_type());
    _cdata->_data.swap(new_data);

    _cdata->_modified = Geom::get_next_modified();
    _cdata->_data_full_size = _cdata->_data.size();

    if (get_current_thread()->get_pipeline_stage() == 0) {
      _object->set_ram_class(RC_resident);
      _object->set_lru_size(_cdata->_data_full_size);
    }
    return true;
  }
  
  return false;
}

////////////////////////////////////////////////////////////////////
//     Function: GeomVertexArrayDataHandle::copy_data_from
//       Access: Public
//  Description: Copies the entire data array from the other object.
////////////////////////////////////////////////////////////////////
void GeomVertexArrayDataHandle::
copy_data_from(const GeomVertexArrayDataHandle *other) {
  nassertv(_writable);
  check_resident();
  other->check_resident();

  _cdata->_data = other->_cdata->_data;
  _cdata->_modified = Geom::get_next_modified();
  _cdata->_data_full_size = _cdata->_data.size();

  if (get_current_thread()->get_pipeline_stage() == 0) {
    _object->set_ram_class(RC_resident);
    _object->set_lru_size(_cdata->_data_full_size);
  }
}

////////////////////////////////////////////////////////////////////
//     Function: GeomVertexArrayDataHandle::copy_subdata_from
//       Access: Public
//  Description: Copies a portion of the data array from the other
//               object into a portion of the data array of this
//               object.  If to_size != from_size, the size of this
//               data array is adjusted accordingly.
////////////////////////////////////////////////////////////////////
void GeomVertexArrayDataHandle::
copy_subdata_from(size_t to_start, size_t to_size,
                  const GeomVertexArrayDataHandle *other,
                  size_t from_start, size_t from_size) {
  nassertv(_writable);
  check_resident();
  other->check_resident();

  GeomVertexArrayData::Data &to_v = _cdata->_data;
  to_start = min(to_start, to_v.size());
  to_size = min(to_size, to_v.size() - to_start);

  from_start = min(from_start, other->_cdata->_data.size());
  from_size = min(from_size, other->_cdata->_data.size() - from_start);

  if (from_size < to_size) {
    // Reduce the array.
    to_v.erase(to_v.begin() + to_start + from_size, to_v.begin() + to_start + to_size);

  } else if (to_size < from_size) {
    // Expand the array.
    to_v.insert(to_v.begin() + to_start + to_size, from_size - to_size, char());
  }

  // Now copy the data.
  memcpy(&to_v[0] + to_start, other->get_pointer() + from_start, from_size);
  _cdata->_modified = Geom::get_next_modified();
  _cdata->_data_full_size = _cdata->_data.size();

  if (get_current_thread()->get_pipeline_stage() == 0) {
    _object->set_ram_class(RC_resident);
    _object->set_lru_size(_cdata->_data_full_size);
  }
}

