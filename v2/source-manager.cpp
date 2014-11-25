// vim: set ts=2 sw=2 tw=99 et:
// 
// Copyright (C) 2012-2014 David Anderson
// 
// This file is part of SourcePawn.
// 
// SourcePawn is free software: you can redistribute it and/or modify it under
// the terms of the GNU General Public License as published by the Free
// Software Foundation, either version 3 of the License, or (at your option)
// any later version.
// 
// SourcePawn is distributed in the hope that it will be useful, but WITHOUT ANY
// WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
// FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.
// 
// You should have received a copy of the GNU General Public License along with
// SourcePawn. If not, see http://www.gnu.org/licenses/.
#include "source-manager.h"
#include "compile-context.h"
#include <stdio.h>
#include <am-bits.h>

using namespace ke;

class FileReader
{
 public:
  FileReader(ReportingContext &cc, const char *path)
   : cc_(cc),
     path_(path),
     fp_(nullptr)
  {
    if ((fp_ = fopen(path, "rb")) == nullptr)
      cc.reportError(Message_FileReadError);
  }
  ~FileReader() {
    if (fp_)
      fclose(fp_);
  }

  bool isValid() const {
    return !!fp_;
  }

  bool read(char **ptr, uint32_t *lengthp) {
    if (fseek(fp_, 0, SEEK_END) == -1) {
      cc_.reportError(Message_FileReadError, path_);
      return false;
    }

    long size = ftell(fp_);
    if (size == -1 || fseek(fp_, 0, SEEK_SET) == -1) {
      cc_.reportError(Message_FileReadError, path_);
      return false;
    }

    if (size_t(size) > kMaxTotalSourceFileLength) {
      cc_.reportError(Message_FileTooLarge, path_);
      return false;
    }

    AutoArray<char> buffer(new char[size + 1]);
    if (!buffer) {
      cc_.reportFatal(rmsg::outofmemory);
      return false;
    }

    if (fread((char *)buffer, 1, size, fp_) != size_t(size)) {
      cc_.reportError(Message_FileReadError, path_);
      return false;
    }

    *ptr = buffer.forget();
    *lengthp = uint32_t(size);
    return true;
  }

 private:
  ReportingContext &cc_;
  const char *path_;
  FILE *fp_;
};

void
SourceFile::computeLineCache()
{
  Vector<uint32_t> lines;

  lines.append(0);
  for (uint32_t i = 0; i < length_; i++) {
    if (chars_[i] == '\r' || chars_[i] == '\n') {
      if (chars_[i] == '\r') {
        // Detect \r\n.
        if (i + 1 < length_ && chars_[i + 1] == '\n')
          i++;
      }
      lines.append(i + 1);
    }
  }

  line_cache_ = new LineExtents(lines.length());
  if (!line_cache_->initialize()) {
    line_cache_ = nullptr;
    return;
  }

  memcpy(line_cache_->buffer(), lines.buffer(), sizeof(uint32_t) * lines.length());
}

SourceManager::SourceManager(CompileContext &cc)
 : cc_(cc),
   next_source_id_(1),
   last_lookup_(0)
{
}

PassRef<SourceFile>
SourceManager::open(ReportingContext &cc, const char *path)
{
  Atom *atom = cc_.add(path);
  AtomMap<Ref<SourceFile>>::Insert p = file_cache_.findForAdd(atom);
  if (p.found())
    return p->value;

  FileReader reader(cc, path);
  if (!reader.isValid())
    return nullptr;

  uint32_t length;
  AutoArray<char> chars;
  if (!reader.read(chars.address(), &length))
    return nullptr;

  Ref<SourceFile> file = new SourceFile(chars.forget(), length, path);
  file_cache_.add(p, atom, file);
  return file;
}

bool
SourceManager::trackExtents(uint32_t length, size_t *index)
{
  // We allocate an extra 2 so we can refer to the end-of-file position without
  // colling with the next range.
  uint32_t next_source_id;
  if (!TryUint32Add(next_source_id_, length, &next_source_id) ||
      !TryUint32Add(next_source_id, 2, &next_source_id))
  {
    return false;
  }

  *index = locations_.length();

  LREntry tracker;
  tracker.id = next_source_id_;
  locations_.append(tracker);

  next_source_id_ = next_source_id;
  return true;
}

LREntry
SourceManager::trackFile(const SourceLocation &from, Ref<SourceFile> file)
{
  size_t loc_index;
  if (!trackExtents(file->length(), &loc_index)) {
    cc_.reportError(from, Message_RanOutOfFileSourceLocations, file->path());
    return LREntry();
  }

  locations_[loc_index].init(from, file);
  return locations_[loc_index];
}

LREntry
SourceManager::trackMacro(const SourceLocation &from, Macro *macro)
{
  size_t loc_index;
  if (!trackExtents(macro->length(), &loc_index)) {
    cc_.reportError(from, Message_RanOutOfMacroSourceLocations, macro->name->chars());
    return LREntry();
  }

  locations_[loc_index].init(from, macro);
  return locations_[loc_index];
}

bool
SourceManager::findLocation(const SourceLocation &loc, size_t *aIndex)
{
  if (!loc.isSet())
    return false;

  if (last_lookup_ < locations_.length() && locations_[last_lookup_].owns(loc)) {
    *aIndex = last_lookup_;
    return true;
  }

  // We should not allocate ids >= the next id.
  assert(loc.offset() < next_source_id_);

  // Binary search.
  size_t lower = 0;
  size_t upper = locations_.length();
  while (lower < upper) {
    size_t index = (lower + upper) / 2;

    LREntry &range = locations_[index];
    if (loc.offset() < range.id) {
      upper = index;
    } else if (loc.offset() > range.id + range.length() + 1) {
      // Note +1 for the terminal offset.
      lower = index + 1;
    } else {
      assert(range.owns(loc));

      // Update cache.
      last_lookup_ = index;

      *aIndex = index;
      return true;
    }
  }

  // What happened?
  assert(false);
  return false;
}

SourceLocation
SourceManager::normalize(const SourceLocation &aLoc)
{
  SourceLocation loc = aLoc;
  while (loc.isInMacro()) {
    size_t loc_index;
    if (!findLocation(loc, &loc_index))
      return SourceLocation();
    loc = locations_[loc_index].getParent();
  }
  return loc;
}

PassRef<SourceFile>
SourceManager::getSource(const SourceLocation &aLoc)
{
  SourceLocation loc = normalize(aLoc);

  size_t loc_index;
  if (!findLocation(loc, &loc_index))
    return nullptr;

  return locations_[loc_index].getFile();
}

unsigned
SourceManager::getLine(const SourceLocation &aLoc)
{
  SourceLocation loc = normalize(aLoc);

  size_t loc_index;
  if (!findLocation(loc, &loc_index))
    return 0;

  return getLine(locations_[loc_index], loc);
}

unsigned
SourceManager::getLine(const LREntry &range, const SourceLocation &loc)
{
  SourceFile *file = range.getFile();

  // Note: we don't OOM check this, since we don't oom check anything.
  if (!file->lineCache())
    file->computeLineCache();

  uint32_t pos = loc.offset() - range.id;
  assert(pos <= file->length());

  // If the position is at end-of-file, return the last line number.
  LineExtents *lines = file->lineCache();
  if (pos == file->length())
    return lines->length();

  uint32_t lower = 0;
  uint32_t upper = lines->length();
  while (lower < upper) {
    uint32_t index = (lower + upper) / 2;

    uint32_t line_start = lines->at(index);
    if (pos < line_start) {
      upper = index;
      continue;
    }

    // The range should be (start, end].
    uint32_t line_end = (index < lines->length() - 1)
                        ? lines->at(index + 1)
                        : file->length();
    if (pos >= line_end) {
      lower = index + 1;
      continue;
    }

    // Either the id is the first character of a line, or before the first
    // character of the next line, or it should be the terminal offset.
    assert(pos >= line_start && pos < line_end);
    return index + 1;
  }

  assert(false);
  return 0;
}

unsigned
SourceManager::getCol(const SourceLocation &loc)
{
  return getCol(loc, getLine(loc));
}

unsigned
SourceManager::getCol(const SourceLocation &aLoc, unsigned line)
{
  SourceLocation loc = normalize(aLoc);

  size_t loc_index;
  if (!findLocation(loc, &loc_index))
    return 0;

  SourceFile *file = locations_[loc_index].getFile();

  // Cached and returned lines are + 1, but the line cache starts at 0.
  line = line - 1;
  assert(line < file->lineCache()->length());

  uint32_t pos = loc.offset() - locations_[loc_index].id;
  uint32_t line_start = file->lineCache()->at(line);

#if !defined(NDEBUG)
  uint32_t line_end = (line < file->lineCache()->length() - 1)
                      ? file->lineCache()->at(line + 1)
                      : file->length() + 1;
  assert(pos >= line_start && pos < line_end);
#endif

  return pos - line_start + 1;
}

FullSourceRef
SourceManager::decode(const SourceLocation &loc)
{
  FullSourceRef ref;
  ref.source = getSource(loc);
  ref.line = getLine(loc);
  ref.col = getCol(loc, ref.line);
  return ref;
}

bool
SourceManager::sameSourceId(const SourceLocation &a, const SourceLocation &b)
{
  if (!a.isSet() && !b.isSet())
    return true;

  size_t loc_a, loc_b;
  if (!findLocation(a, &loc_a) || !findLocation(b, &loc_b))
    return false;
  return loc_a == loc_b;
}

bool
SourceManager::areLocationsInsertedOnSameLine(const SourceLocation &aLocA,
                                              const SourceLocation &aLocB)
{
  SourceLocation a = normalize(aLocA);
  SourceLocation b = normalize(aLocB);

  // This can occur with builtin tokens.
  if (!a.isSet() && !b.isSet())
    return true;

  size_t loc_a, loc_b;
  if (!findLocation(a, &loc_a) || !findLocation(b, &loc_b))
    return false;
  if (loc_a != loc_b)
    return false;

  unsigned line_a = getLine(locations_[loc_a], a);
  unsigned line_b = getLine(locations_[loc_b], b);
  return line_a && line_b && line_a == line_b;
}
