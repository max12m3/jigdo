/* $Id$ -*- C++ -*-
  __   _
  |_) /|  Copyright (C) 2001-2002  |  richard@
  | \/�|  Richard Atterer          |  atterer.net
  � '` �
  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License, version 2. See
  the file COPYING for details.

  Zlib compression layer which integrates with C++ streams

*/

#include <config.h>

#include <errno.h>
#include <string.h>
#include <algorithm>
#include <fstream>
#include <new>

#include <md5sum.hh>
#include <serialize.hh>
#include <string.hh>
#include <zstream.hh>

#ifndef DEBUG_ZSTREAM
#  define DEBUG_ZSTREAM (DEBUG && 0)
#endif
//______________________________________________________________________

namespace {

  // Turn zlib error codes/messages into C++ exceptions
  void throwZerror(int status, const char* zmsg) {
    string m;
    if (zmsg != 0) m += zmsg;
    Assert(status != Z_OK);
    switch (status) {
    case Z_OK:
      break;
    case Z_ERRNO:
      if (!m.empty() && errno != 0) m += " - ";
      if (errno != 0) m += strerror(errno);
      throw Zerror(Z_ERRNO, m);
      break;
    case Z_MEM_ERROR:
      throw bad_alloc();
      break;
      // NB: fallthrough:
    case Z_STREAM_ERROR:
      if (m.empty()) m = "zlib Z_STREAM_ERROR";
    case Z_DATA_ERROR:
      if (m.empty()) m = "zlib Z_DATA_ERROR";
    case Z_BUF_ERROR:
      if (m.empty()) m = "zlib Z_BUF_ERROR";
    case Z_VERSION_ERROR:
      if (m.empty()) m = "zlib Z_VERSION_ERROR";
    default:
      throw Zerror(status, m);
    }
  }

}
//________________________________________

void Zobstream::throwZerror(int status, const char* zmsg) {
  ::throwZerror(status, zmsg);
}
void Zibstream::throwZerror(int status, const char* zmsg) {
  ::throwZerror(status, zmsg);
}
//______________________________________________________________________

void Zobstream::open(bostream& s, size_t chunkLimit, int level,
                     int windowBits, int memLevel, size_t todoBufSz) {
  Assert(!is_open());
  todoBufSize = (MIN_TODOBUF_SIZE > todoBufSz ? MIN_TODOBUF_SIZE :todoBufSz);
  chunkLim = chunkLimit;

  todoCount = 0;
  todoBuf = new byte[todoBufSize];
  z.next_in = z.next_out = 0;
  z.avail_out = (zipBuf == 0 ? 0 : ZIPDATA_SIZE);
  z.total_in = 0;
  int status = deflateInit2(&z, level, Z_DEFLATED, windowBits, memLevel,
                            Z_DEFAULT_STRATEGY);
  if (status != Z_OK) throwZerror(status, z.msg);
  stream = &s;
}
//________________________________________

void Zobstream::close() {
  if (!is_open()) return;
  //cerr<<"zobstream:close()" << endl;
  zip(todoBuf, todoCount, Z_FINISH); // Flush out remain. buffer contents

  int status = deflateEnd(&z);
  zipBufLast = zipBuf;

  // Deallocate memory
  delete[] todoBuf;
  todoBuf = 0;
  todoBufSize = todoCount = 0; // Important; cf Zobstream()
  stream = 0;

  /* Only report errors *after* marking the stream as closed, to avoid
     another exception being thrown when the Zobstream object goes out
     of scope and ~Zobstream calls close() again. */
  if (status != Z_OK) throwZerror(status, z.msg);
}
//______________________________________________________________________

// Write compressed, flushed data to output stream
void Zobstream::writeZipped() {
# if DEBUG_ZSTREAM
  cerr << "Zobstream: Writing " << z.total_out << " bytes compressed, was "
       << z.total_in << " uncompressed" << endl;
# endif

  // #Bytes     Value   Description
  // ----------------------------------------------------------------------
  //  4       dataID  "ID for the part: 'DATA' = the hex bytes 44 41 54 41"
  //  6       dataLen "Length of part, i.e. length of compressed data + 16"
  //  6       dataUnc "Number of bytes of *uncompressed* data of this part"
  // dataLen-16       "Compressed data"
  byte buf[16];
  byte* p = buf;
  serialize4(0x41544144u, p); // DATA
  uint64 l = z.total_out + 16;
  serialize6(l, p + 4);
  l = z.total_in;
  serialize6(l, p + 10);
  writeBytes(*stream, buf, 16);
  if (!stream->good())
    throw Zerror(0, string(_("Could not write template data")));
  if (md5sum != 0) md5sum->update(buf, 16);

  ZipData* zd = zipBuf;
  size_t len;
  while (true) {
    Paranoid(zd != 0);
    len = (z.total_out < ZIPDATA_SIZE ? z.total_out : ZIPDATA_SIZE);
    writeBytes(*stream, zd->data, len);
    if (md5sum != 0) md5sum->update(zd->data, len);
    if (!stream->good())
      throw Zerror(0, string(_("Could not write template data")));
    zd = zd->next;
    if (len < ZIPDATA_SIZE || zd == 0) break;
    z.total_out -= ZIPDATA_SIZE;
  }

  zipBufLast = zipBuf;
  z.next_out = zipBuf->data;
  z.avail_out = ZIPDATA_SIZE;
  z.total_in = z.total_out = 0;
  // NB: next_in, avail_in are left alone

  int status = deflateReset(&z);
  if (status != Z_OK) throwZerror(status, z.msg);
}
//________________________________________

void Zobstream::zip(byte* start, size_t len, int flush) {
  Assert(is_open());

  // If big enough, finish and write out this chunk
  if (z.total_out > chunkLim) flush = Z_FINISH;

  // true <=> must call deflate() at least once
  bool callZlibOnce = (flush != Z_NO_FLUSH);

  z.next_in = start;
  z.avail_in = len;
  while (z.avail_in != 0 || z.avail_out == 0 || callZlibOnce) {
    callZlibOnce = false;

    if (z.avail_out == 0) {
      // Get another output buffer object
      ZipData* zd;
      if (zipBufLast == 0 || zipBufLast->next == 0) {
        zd = new ZipData();
        if (zipBuf == 0) zipBuf = zd;
        if (zipBufLast != 0) zipBufLast->next = zd;
      } else {
        zd = zipBufLast->next;
      }
      zipBufLast = zd;
      z.next_out = zd->data;
      z.avail_out = ZIPDATA_SIZE;
      //cerr << "Zob: new ZipData @ " << &zd->data << endl;
    }

    int status = deflate(&z, flush); // Call zlib
    //cerr << "zip(" << (void*)start << ", " << len << ", " << flush
    //     << ") returned " << status << endl;
    if (status == Z_STREAM_END
//      || (status == Z_OK && z.total_out > chunkLim)
        || (flush == Z_FULL_FLUSH && z.total_in != 0)) {
      writeZipped();
      flush = Z_NO_FLUSH;
    }

    if (status == Z_STREAM_END) continue;
    if (status != Z_OK) throwZerror(status, z.msg);
  }

  todoCount = 0;
}
//______________________________________________________________________

Zobstream& Zobstream::put(uint32 x) {
  if (todoCount > todoBufSize - 4) zip(todoBuf, todoCount);
  todoBuf[todoCount] = static_cast<byte>(x & 0xff);
  ++todoCount;
  todoBuf[todoCount] = static_cast<byte>((x >> 8) & 0xff);
  ++todoCount;
  todoBuf[todoCount] = static_cast<byte>((x >> 16) & 0xff);
  ++todoCount;
  todoBuf[todoCount] = static_cast<byte>((x >> 24) & 0xff);
  ++todoCount;
  return *this;
}
//______________________________________________________________________

void Zibstream::open(bistream& s) {
  Assert(!is_open());
  Paranoid(buf == 0);
  buf = new byte[bufSize];
  z.next_in = z.next_out = 0;
  z.avail_in = z.total_out = 0;
  int status = inflateInit(&z);
  if (status != Z_OK) throwZerror(status, z.msg);
  dataLen = dataUnc = 0;
  stream = &s;
}

void Zibstream::close() {
  if (!is_open()) return;

  int status = inflateEnd(&z);

  // Deallocate memory
  delete[] buf;
  buf = 0;
  stream = 0;

  /* Only report errors *after* marking the stream as closed, to avoid
     another exception being thrown when the Zibstream object goes out
     of scope and ~Zibstream calls close() again. */
  if (status != Z_OK) throwZerror(status, z.msg);
}
//________________________________________

Zibstream& Zibstream::read(byte* dest, size_t n) {
  gcountVal = 0; // in case n == 0
  if (!good()) return *this;
  z.next_out = dest;
  z.avail_out = n;

  //cerr << "Zibstream::read: " << n << " to read, avail_in=" << z.avail_in
  //     << endl;
  SerialIstreamIterator in(*stream);
  while (z.avail_out > 0) {
    //____________________

    /* If possible, uncompress into destination buffer. Handling this
       case first for speed */
    if (z.avail_in != 0) {
      byte* oldNextOut = z.next_out;
      int status = inflate(&z, Z_NO_FLUSH);
      gcountVal = z.next_out - dest;
      dataUnc -= z.next_out - oldNextOut;
#     if DEBUG_ZSTREAM
      cerr << "Zibstream::read: avail_out=" << z.avail_out
           << " dataLen=" << dataLen
           << " dataUnc=" << dataUnc << " status=" << status
           << " - inflated " << z.next_out - oldNextOut << endl;
#     endif
      Assert(dataUnc > 0 || (status == Z_STREAM_END || status == Z_OK));
      if (z.avail_out == 0) break;
      if (status != Z_OK && status != Z_STREAM_END)
        throwZerror(status, z.msg);
      continue;
    }
    //____________________

    // Need to read another DATA part?
    if (dataLen == 0) {
      Assert(dataUnc == 0);
      const char* hdr = "DATA";
      const char* cur = hdr;
      byte x;
      while (*cur != '\0' && *stream) {
        *stream >> x; // Any errors handled below, after end of while()
        if (*cur != x) { // Reached end of file or non-DATA part
          stream->seekg(hdr - cur, ios::cur);
          delete[] buf;
          buf = 0; // Causes fail() == true
          throw Zerror(0, string(_("Corrupted input data")));
        }
        ++cur;
      }
      unserialize6(dataLen, in);
      dataLen -= 16;
      unserialize6(dataUnc, in);
#     if 0
      cerr << "Zibstream::read: avail_out=" << z.avail_out
           << " dataLen=" << dataLen
           << " dataUnc=" << dataUnc << " - new DATA part" << endl;
#     endif
      if (dataUnc == 0 || !*stream) {
        delete[] buf;
        buf = 0;
        throw Zerror(0, string(_("Corrupted input data")));
      }
      int status = inflateReset(&z);
      if (status != Z_OK) throwZerror(status, z.msg);
    }
    //____________________

    // Read data from file into buffer?
    size_t toRead = (dataLen < bufSize ? dataLen : bufSize);
    byte* b = &buf[0];
    z.next_in = b;
    z.avail_in = toRead;
    dataLen -= toRead;
    while (*stream && toRead > 0) {
      readBytes(*stream, b, toRead);
      size_t n = stream->gcount();
      b += n;
      toRead -= n;
    }
#   if 0
    cerr << "Zibstream::read: avail_out=" << z.avail_out
         << " dataLen=" << dataLen
         << " dataUnc=" << dataUnc << " - read "
         << z.avail_in << " from file to buf" << endl;
#   endif
    if (!*stream) {
      delete[] buf;
      buf = 0;
      string err = subst(_("Error reading compressed data - %1"),
                         strerror(errno));
      throw Zerror(0, err);
    }
    //____________________

  } // endwhile (n > 0)

# if 0
  cerr << "Zibstream::read: avail_out=" << z.avail_out
       << " dataLen=" << dataLen
       << " dataUnc=" << dataUnc << " - returns, gcount=" << gcountVal
       << " avail_in=" << z.avail_in
       << endl;
# endif

  return *this;
}