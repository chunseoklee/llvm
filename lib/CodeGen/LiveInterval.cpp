//===-- LiveInterval.cpp - Live Interval Representation -------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file implements the LiveRange and LiveInterval classes.  Given some
// numbering of each the machine instructions an interval [i, j) is said to be a
// live range for register v if there is no instruction with number j' >= j
// such that v is live at j' and there is no instruction with number i' < i such
// that v is live at i'. In this implementation ranges can have holes,
// i.e. a range might look like [1,20), [50,65), [1000,1001).  Each
// individual segment is represented as an instance of LiveRange::Segment,
// and the whole range is represented as an instance of LiveRange.
//
//===----------------------------------------------------------------------===//

#include "llvm/CodeGen/LiveInterval.h"

#include "PHIEliminationUtils.h"
#include "RegisterCoalescer.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallSet.h"
#include "llvm/CodeGen/LiveIntervalAnalysis.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/CodeGen/MachineRegisterInfo.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Target/TargetInstrInfo.h"
#include "llvm/Target/TargetRegisterInfo.h"
#include <algorithm>
using namespace llvm;

namespace {
//===----------------------------------------------------------------------===//
// Implementation of various methods necessary for calculation of live ranges.
// The implementation of the methods abstracts from the concrete type of the
// segment collection.
//
// Implementation of the class follows the Template design pattern. The base
// class contains generic algorithms that call collection-specific methods,
// which are provided in concrete subclasses. In order to avoid virtual calls
// these methods are provided by means of C++ template instantiation.
// The base class calls the methods of the subclass through method impl(),
// which casts 'this' pointer to the type of the subclass.
//
//===----------------------------------------------------------------------===//

template <typename ImplT, typename IteratorT, typename CollectionT>
class CalcLiveRangeUtilBase {
protected:
  LiveRange *LR;

protected:
  CalcLiveRangeUtilBase(LiveRange *LR) : LR(LR) {}

public:
  typedef LiveRange::Segment Segment;
  typedef IteratorT iterator;

  VNInfo *createDeadDef(SlotIndex Def, VNInfo::Allocator &VNInfoAllocator) {
    assert(!Def.isDead() && "Cannot define a value at the dead slot");

    iterator I = impl().find(Def);
    if (I == segments().end()) {
      VNInfo *VNI = LR->getNextValue(Def, VNInfoAllocator);
      impl().insertAtEnd(Segment(Def, Def.getDeadSlot(), VNI));
      return VNI;
    }

    Segment *S = segmentAt(I);
    if (SlotIndex::isSameInstr(Def, S->start)) {
      assert(S->valno->def == S->start && "Inconsistent existing value def");

      // It is possible to have both normal and early-clobber defs of the same
      // register on an instruction. It doesn't make a lot of sense, but it is
      // possible to specify in inline assembly.
      //
      // Just convert everything to early-clobber.
      Def = std::min(Def, S->start);
      if (Def != S->start)
        S->start = S->valno->def = Def;
      return S->valno;
    }
    assert(SlotIndex::isEarlierInstr(Def, S->start) && "Already live at def");
    VNInfo *VNI = LR->getNextValue(Def, VNInfoAllocator);
    segments().insert(I, Segment(Def, Def.getDeadSlot(), VNI));
    return VNI;
  }

  VNInfo *extendInBlock(SlotIndex StartIdx, SlotIndex Use) {
    if (segments().empty())
      return nullptr;
    iterator I =
        impl().findInsertPos(Segment(Use.getPrevSlot(), Use, nullptr));
    if (I == segments().begin())
      return nullptr;
    --I;
    if (I->end <= StartIdx)
      return nullptr;
    if (I->end < Use)
      extendSegmentEndTo(I, Use);
    return I->valno;
  }

  /// This method is used when we want to extend the segment specified
  /// by I to end at the specified endpoint. To do this, we should
  /// merge and eliminate all segments that this will overlap
  /// with. The iterator is not invalidated.
  void extendSegmentEndTo(iterator I, SlotIndex NewEnd) {
    assert(I != segments().end() && "Not a valid segment!");
    Segment *S = segmentAt(I);
    VNInfo *ValNo = I->valno;

    // Search for the first segment that we can't merge with.
    iterator MergeTo = std::next(I);
    for (; MergeTo != segments().end() && NewEnd >= MergeTo->end; ++MergeTo)
      assert(MergeTo->valno == ValNo && "Cannot merge with differing values!");

    // If NewEnd was in the middle of a segment, make sure to get its endpoint.
    S->end = std::max(NewEnd, std::prev(MergeTo)->end);

    // If the newly formed segment now touches the segment after it and if they
    // have the same value number, merge the two segments into one segment.
    if (MergeTo != segments().end() && MergeTo->start <= I->end &&
        MergeTo->valno == ValNo) {
      S->end = MergeTo->end;
      ++MergeTo;
    }

    // Erase any dead segments.
    segments().erase(std::next(I), MergeTo);
  }

  /// This method is used when we want to extend the segment specified
  /// by I to start at the specified endpoint.  To do this, we should
  /// merge and eliminate all segments that this will overlap with.
  iterator extendSegmentStartTo(iterator I, SlotIndex NewStart) {
    assert(I != segments().end() && "Not a valid segment!");
    Segment *S = segmentAt(I);
    VNInfo *ValNo = I->valno;

    // Search for the first segment that we can't merge with.
    iterator MergeTo = I;
    do {
      if (MergeTo == segments().begin()) {
        S->start = NewStart;
        segments().erase(MergeTo, I);
        return I;
      }
      assert(MergeTo->valno == ValNo && "Cannot merge with differing values!");
      --MergeTo;
    } while (NewStart <= MergeTo->start);

    // If we start in the middle of another segment, just delete a range and
    // extend that segment.
    if (MergeTo->end >= NewStart && MergeTo->valno == ValNo) {
      segmentAt(MergeTo)->end = S->end;
    } else {
      // Otherwise, extend the segment right after.
      ++MergeTo;
      Segment *MergeToSeg = segmentAt(MergeTo);
      MergeToSeg->start = NewStart;
      MergeToSeg->end = S->end;
    }

    segments().erase(std::next(MergeTo), std::next(I));
    return MergeTo;
  }

  iterator addSegment(Segment S) {
    SlotIndex Start = S.start, End = S.end;
    iterator I = impl().findInsertPos(S);

    // If the inserted segment starts in the middle or right at the end of
    // another segment, just extend that segment to contain the segment of S.
    if (I != segments().begin()) {
      iterator B = std::prev(I);
      if (S.valno == B->valno) {
        if (B->start <= Start && B->end >= Start) {
          extendSegmentEndTo(B, End);
          return B;
        }
      } else {
        // Check to make sure that we are not overlapping two live segments with
        // different valno's.
        assert(B->end <= Start &&
               "Cannot overlap two segments with differing ValID's"
               " (did you def the same reg twice in a MachineInstr?)");
      }
    }

    // Otherwise, if this segment ends in the middle of, or right next
    // to, another segment, merge it into that segment.
    if (I != segments().end()) {
      if (S.valno == I->valno) {
        if (I->start <= End) {
          I = extendSegmentStartTo(I, Start);

          // If S is a complete superset of a segment, we may need to grow its
          // endpoint as well.
          if (End > I->end)
            extendSegmentEndTo(I, End);
          return I;
        }
      } else {
        // Check to make sure that we are not overlapping two live segments with
        // different valno's.
        assert(I->start >= End &&
               "Cannot overlap two segments with differing ValID's");
      }
    }

    // Otherwise, this is just a new segment that doesn't interact with
    // anything.
    // Insert it.
    return segments().insert(I, S);
  }

private:
  ImplT &impl() { return *static_cast<ImplT *>(this); }

  CollectionT &segments() { return impl().segmentsColl(); }

  Segment *segmentAt(iterator I) { return const_cast<Segment *>(&(*I)); }
};

//===----------------------------------------------------------------------===//
//   Instantiation of the methods for calculation of live ranges
//   based on a segment vector.
//===----------------------------------------------------------------------===//

class CalcLiveRangeUtilVector;
typedef CalcLiveRangeUtilBase<CalcLiveRangeUtilVector, LiveRange::iterator,
                              LiveRange::Segments> CalcLiveRangeUtilVectorBase;

class CalcLiveRangeUtilVector : public CalcLiveRangeUtilVectorBase {
public:
  CalcLiveRangeUtilVector(LiveRange *LR) : CalcLiveRangeUtilVectorBase(LR) {}

private:
  friend CalcLiveRangeUtilVectorBase;

  LiveRange::Segments &segmentsColl() { return LR->segments; }

  void insertAtEnd(const Segment &S) { LR->segments.push_back(S); }

  iterator find(SlotIndex Pos) { return LR->find(Pos); }

  iterator findInsertPos(Segment S) {
    return std::upper_bound(LR->begin(), LR->end(), S.start);
  }
};

//===----------------------------------------------------------------------===//
//   Instantiation of the methods for calculation of live ranges
//   based on a segment set.
//===----------------------------------------------------------------------===//

class CalcLiveRangeUtilSet;
typedef CalcLiveRangeUtilBase<CalcLiveRangeUtilSet,
                              LiveRange::SegmentSet::iterator,
                              LiveRange::SegmentSet> CalcLiveRangeUtilSetBase;

class CalcLiveRangeUtilSet : public CalcLiveRangeUtilSetBase {
public:
  CalcLiveRangeUtilSet(LiveRange *LR) : CalcLiveRangeUtilSetBase(LR) {}

private:
  friend CalcLiveRangeUtilSetBase;

  LiveRange::SegmentSet &segmentsColl() { return *LR->segmentSet; }

  void insertAtEnd(const Segment &S) {
    LR->segmentSet->insert(LR->segmentSet->end(), S);
  }

  iterator find(SlotIndex Pos) {
    iterator I =
        LR->segmentSet->upper_bound(Segment(Pos, Pos.getNextSlot(), nullptr));
    if (I == LR->segmentSet->begin())
      return I;
    iterator PrevI = std::prev(I);
    if (Pos < (*PrevI).end)
      return PrevI;
    return I;
  }

  iterator findInsertPos(Segment S) {
    iterator I = LR->segmentSet->upper_bound(S);
    if (I != LR->segmentSet->end() && !(S.start < *I))
      ++I;
    return I;
  }
};
} // namespace

//===----------------------------------------------------------------------===//
//   LiveRange methods
//===----------------------------------------------------------------------===//

LiveRange::iterator LiveRange::find(SlotIndex Pos) {
  // This algorithm is basically std::upper_bound.
  // Unfortunately, std::upper_bound cannot be used with mixed types until we
  // adopt C++0x. Many libraries can do it, but not all.
  if (empty() || Pos >= endIndex())
    return end();
  iterator I = begin();
  size_t Len = size();
  do {
    size_t Mid = Len >> 1;
    if (Pos < I[Mid].end) {
      Len = Mid;
    } else {
      I += Mid + 1;
      Len -= Mid + 1;
    }
  } while (Len);
  return I;
}

VNInfo *LiveRange::createDeadDef(SlotIndex Def,
                                  VNInfo::Allocator &VNInfoAllocator) {
  // Use the segment set, if it is available.
  if (segmentSet != nullptr)
    return CalcLiveRangeUtilSet(this).createDeadDef(Def, VNInfoAllocator);
  // Otherwise use the segment vector.
  return CalcLiveRangeUtilVector(this).createDeadDef(Def, VNInfoAllocator);
}

// overlaps - Return true if the intersection of the two live ranges is
// not empty.
//
// An example for overlaps():
//
// 0: A = ...
// 4: B = ...
// 8: C = A + B ;; last use of A
//
// The live ranges should look like:
//
// A = [3, 11)
// B = [7, x)
// C = [11, y)
//
// A->overlaps(C) should return false since we want to be able to join
// A and C.
//
bool LiveRange::overlapsFrom(const LiveRange& other,
                             const_iterator StartPos) const {
  assert(!empty() && "empty range");
  const_iterator i = begin();
  const_iterator ie = end();
  const_iterator j = StartPos;
  const_iterator je = other.end();

  assert((StartPos->start <= i->start || StartPos == other.begin()) &&
         StartPos != other.end() && "Bogus start position hint!");

  if (i->start < j->start) {
    i = std::upper_bound(i, ie, j->start);
    if (i != begin()) --i;
  } else if (j->start < i->start) {
    ++StartPos;
    if (StartPos != other.end() && StartPos->start <= i->start) {
      assert(StartPos < other.end() && i < end());
      j = std::upper_bound(j, je, i->start);
      if (j != other.begin()) --j;
    }
  } else {
    return true;
  }

  if (j == je) return false;

  while (i != ie) {
    if (i->start > j->start) {
      std::swap(i, j);
      std::swap(ie, je);
    }

    if (i->end > j->start)
      return true;
    ++i;
  }

  return false;
}

bool LiveRange::overlaps(const LiveRange &Other, const CoalescerPair &CP,
                         const SlotIndexes &Indexes) const {
  assert(!empty() && "empty range");
  if (Other.empty())
    return false;

  // Use binary searches to find initial positions.
  const_iterator I = find(Other.beginIndex());
  const_iterator IE = end();
  if (I == IE)
    return false;
  const_iterator J = Other.find(I->start);
  const_iterator JE = Other.end();
  if (J == JE)
    return false;

  for (;;) {
    // J has just been advanced to satisfy:
    assert(J->end >= I->start);
    // Check for an overlap.
    if (J->start < I->end) {
      // I and J are overlapping. Find the later start.
      SlotIndex Def = std::max(I->start, J->start);
      // Allow the overlap if Def is a coalescable copy.
      if (Def.isBlock() ||
          !CP.isCoalescable(Indexes.getInstructionFromIndex(Def)))
        return true;
    }
    // Advance the iterator that ends first to check for more overlaps.
    if (J->end > I->end) {
      std::swap(I, J);
      std::swap(IE, JE);
    }
    // Advance J until J->end >= I->start.
    do
      if (++J == JE)
        return false;
    while (J->end < I->start);
  }
}

/// overlaps - Return true if the live range overlaps an interval specified
/// by [Start, End).
bool LiveRange::overlaps(SlotIndex Start, SlotIndex End) const {
  assert(Start < End && "Invalid range");
  const_iterator I = std::lower_bound(begin(), end(), End);
  return I != begin() && (--I)->end > Start;
}

bool LiveRange::covers(const LiveRange &Other) const {
  if (empty())
    return Other.empty();

  const_iterator I = begin();
  for (const Segment &O : Other.segments) {
    I = advanceTo(I, O.start);
    if (I == end() || I->start > O.start)
      return false;

    // Check adjacent live segments and see if we can get behind O.end.
    while (I->end < O.end) {
      const_iterator Last = I;
      // Get next segment and abort if it was not adjacent.
      ++I;
      if (I == end() || Last->end != I->start)
        return false;
    }
  }
  return true;
}

/// ValNo is dead, remove it.  If it is the largest value number, just nuke it
/// (and any other deleted values neighboring it), otherwise mark it as ~1U so
/// it can be nuked later.
void LiveRange::markValNoForDeletion(VNInfo *ValNo) {
  if (ValNo->id == getNumValNums()-1) {
    do {
      valnos.pop_back();
    } while (!valnos.empty() && valnos.back()->isUnused());
  } else {
    ValNo->markUnused();
  }
}

/// RenumberValues - Renumber all values in order of appearance and delete the
/// remaining unused values.
void LiveRange::RenumberValues() {
  SmallPtrSet<VNInfo*, 8> Seen;
  valnos.clear();
  for (const Segment &S : segments) {
    VNInfo *VNI = S.valno;
    if (!Seen.insert(VNI).second)
      continue;
    assert(!VNI->isUnused() && "Unused valno used by live segment");
    VNI->id = (unsigned)valnos.size();
    valnos.push_back(VNI);
  }
}

void LiveRange::addSegmentToSet(Segment S) {
  CalcLiveRangeUtilSet(this).addSegment(S);
}

LiveRange::iterator LiveRange::addSegment(Segment S) {
  // Use the segment set, if it is available.
  if (segmentSet != nullptr) {
    addSegmentToSet(S);
    return end();
  }
  // Otherwise use the segment vector.
  return CalcLiveRangeUtilVector(this).addSegment(S);
}

void LiveRange::append(const Segment S) {
  // Check that the segment belongs to the back of the list.
  assert(segments.empty() || segments.back().end <= S.start);
  segments.push_back(S);
}

/// extendInBlock - If this range is live before Kill in the basic
/// block that starts at StartIdx, extend it to be live up to Kill and return
/// the value. If there is no live range before Kill, return NULL.
VNInfo *LiveRange::extendInBlock(SlotIndex StartIdx, SlotIndex Kill) {
  // Use the segment set, if it is available.
  if (segmentSet != nullptr)
    return CalcLiveRangeUtilSet(this).extendInBlock(StartIdx, Kill);
  // Otherwise use the segment vector.
  return CalcLiveRangeUtilVector(this).extendInBlock(StartIdx, Kill);
}

/// Remove the specified segment from this range.  Note that the segment must
/// be in a single Segment in its entirety.
void LiveRange::removeSegment(SlotIndex Start, SlotIndex End,
                              bool RemoveDeadValNo) {
  // Find the Segment containing this span.
  iterator I = find(Start);
  assert(I != end() && "Segment is not in range!");
  assert(I->containsInterval(Start, End)
         && "Segment is not entirely in range!");

  // If the span we are removing is at the start of the Segment, adjust it.
  VNInfo *ValNo = I->valno;
  if (I->start == Start) {
    if (I->end == End) {
      if (RemoveDeadValNo) {
        // Check if val# is dead.
        bool isDead = true;
        for (const_iterator II = begin(), EE = end(); II != EE; ++II)
          if (II != I && II->valno == ValNo) {
            isDead = false;
            break;
          }
        if (isDead) {
          // Now that ValNo is dead, remove it.
          markValNoForDeletion(ValNo);
        }
      }

      segments.erase(I);  // Removed the whole Segment.
    } else
      I->start = End;
    return;
  }

  // Otherwise if the span we are removing is at the end of the Segment,
  // adjust the other way.
  if (I->end == End) {
    I->end = Start;
    return;
  }

  // Otherwise, we are splitting the Segment into two pieces.
  SlotIndex OldEnd = I->end;
  I->end = Start;   // Trim the old segment.

  // Insert the new one.
  segments.insert(std::next(I), Segment(End, OldEnd, ValNo));
}

/// removeValNo - Remove all the segments defined by the specified value#.
/// Also remove the value# from value# list.
void LiveRange::removeValNo(VNInfo *ValNo) {
  if (empty()) return;
  segments.erase(std::remove_if(begin(), end(), [ValNo](const Segment &S) {
    return S.valno == ValNo;
  }), end());
  // Now that ValNo is dead, remove it.
  markValNoForDeletion(ValNo);
}

void LiveRange::join(LiveRange &Other,
                     const int *LHSValNoAssignments,
                     const int *RHSValNoAssignments,
                     SmallVectorImpl<VNInfo *> &NewVNInfo) {
  verify();

  // Determine if any of our values are mapped.  This is uncommon, so we want
  // to avoid the range scan if not.
  bool MustMapCurValNos = false;
  unsigned NumVals = getNumValNums();
  unsigned NumNewVals = NewVNInfo.size();
  for (unsigned i = 0; i != NumVals; ++i) {
    unsigned LHSValID = LHSValNoAssignments[i];
    if (i != LHSValID ||
        (NewVNInfo[LHSValID] && NewVNInfo[LHSValID] != getValNumInfo(i))) {
      MustMapCurValNos = true;
      break;
    }
  }

  // If we have to apply a mapping to our base range assignment, rewrite it now.
  if (MustMapCurValNos && !empty()) {
    // Map the first live range.

    iterator OutIt = begin();
    OutIt->valno = NewVNInfo[LHSValNoAssignments[OutIt->valno->id]];
    for (iterator I = std::next(OutIt), E = end(); I != E; ++I) {
      VNInfo* nextValNo = NewVNInfo[LHSValNoAssignments[I->valno->id]];
      assert(nextValNo && "Huh?");

      // If this live range has the same value # as its immediate predecessor,
      // and if they are neighbors, remove one Segment.  This happens when we
      // have [0,4:0)[4,7:1) and map 0/1 onto the same value #.
      if (OutIt->valno == nextValNo && OutIt->end == I->start) {
        OutIt->end = I->end;
      } else {
        // Didn't merge. Move OutIt to the next segment,
        ++OutIt;
        OutIt->valno = nextValNo;
        if (OutIt != I) {
          OutIt->start = I->start;
          OutIt->end = I->end;
        }
      }
    }
    // If we merge some segments, chop off the end.
    ++OutIt;
    segments.erase(OutIt, end());
  }

  // Rewrite Other values before changing the VNInfo ids.
  // This can leave Other in an invalid state because we're not coalescing
  // touching segments that now have identical values. That's OK since Other is
  // not supposed to be valid after calling join();
  for (Segment &S : Other.segments)
    S.valno = NewVNInfo[RHSValNoAssignments[S.valno->id]];

  // Update val# info. Renumber them and make sure they all belong to this
  // LiveRange now. Also remove dead val#'s.
  unsigned NumValNos = 0;
  for (unsigned i = 0; i < NumNewVals; ++i) {
    VNInfo *VNI = NewVNInfo[i];
    if (VNI) {
      if (NumValNos >= NumVals)
        valnos.push_back(VNI);
      else
        valnos[NumValNos] = VNI;
      VNI->id = NumValNos++;  // Renumber val#.
    }
  }
  if (NumNewVals < NumVals)
    valnos.resize(NumNewVals);  // shrinkify

  // Okay, now insert the RHS live segments into the LHS.
  LiveRangeUpdater Updater(this);
  for (Segment &S : Other.segments)
    Updater.add(S);
}

/// Merge all of the segments in RHS into this live range as the specified
/// value number.  The segments in RHS are allowed to overlap with segments in
/// the current range, but only if the overlapping segments have the
/// specified value number.
void LiveRange::MergeSegmentsInAsValue(const LiveRange &RHS,
                                       VNInfo *LHSValNo) {
  LiveRangeUpdater Updater(this);
  for (const Segment &S : RHS.segments)
    Updater.add(S.start, S.end, LHSValNo);
}

/// MergeValueInAsValue - Merge all of the live segments of a specific val#
/// in RHS into this live range as the specified value number.
/// The segments in RHS are allowed to overlap with segments in the
/// current range, it will replace the value numbers of the overlaped
/// segments with the specified value number.
void LiveRange::MergeValueInAsValue(const LiveRange &RHS,
                                    const VNInfo *RHSValNo,
                                    VNInfo *LHSValNo) {
  LiveRangeUpdater Updater(this);
  for (const Segment &S : RHS.segments)
    if (S.valno == RHSValNo)
      Updater.add(S.start, S.end, LHSValNo);
}

/// MergeValueNumberInto - This method is called when two value nubmers
/// are found to be equivalent.  This eliminates V1, replacing all
/// segments with the V1 value number with the V2 value number.  This can
/// cause merging of V1/V2 values numbers and compaction of the value space.
VNInfo *LiveRange::MergeValueNumberInto(VNInfo *V1, VNInfo *V2) {
  assert(V1 != V2 && "Identical value#'s are always equivalent!");

  // This code actually merges the (numerically) larger value number into the
  // smaller value number, which is likely to allow us to compactify the value
  // space.  The only thing we have to be careful of is to preserve the
  // instruction that defines the result value.

  // Make sure V2 is smaller than V1.
  if (V1->id < V2->id) {
    V1->copyFrom(*V2);
    std::swap(V1, V2);
  }

  // Merge V1 segments into V2.
  for (iterator I = begin(); I != end(); ) {
    iterator S = I++;
    if (S->valno != V1) continue;  // Not a V1 Segment.

    // Okay, we found a V1 live range.  If it had a previous, touching, V2 live
    // range, extend it.
    if (S != begin()) {
      iterator Prev = S-1;
      if (Prev->valno == V2 && Prev->end == S->start) {
        Prev->end = S->end;

        // Erase this live-range.
        segments.erase(S);
        I = Prev+1;
        S = Prev;
      }
    }

    // Okay, now we have a V1 or V2 live range that is maximally merged forward.
    // Ensure that it is a V2 live-range.
    S->valno = V2;

    // If we can merge it into later V2 segments, do so now.  We ignore any
    // following V1 segments, as they will be merged in subsequent iterations
    // of the loop.
    if (I != end()) {
      if (I->start == S->end && I->valno == V2) {
        S->end = I->end;
        segments.erase(I);
        I = S+1;
      }
    }
  }

  // Now that V1 is dead, remove it.
  markValNoForDeletion(V1);

  return V2;
}

void LiveRange::flushSegmentSet() {
  assert(segmentSet != nullptr && "segment set must have been created");
  assert(
      segments.empty() &&
      "segment set can be used only initially before switching to the array");
  segments.append(segmentSet->begin(), segmentSet->end());
  segmentSet = nullptr;
  verify();
}

bool LiveRange::isLiveAtIndexes(ArrayRef<SlotIndex> Slots) const {
  ArrayRef<SlotIndex>::iterator SlotI = Slots.begin();
  ArrayRef<SlotIndex>::iterator SlotE = Slots.end();

  // If there are no regmask slots, we have nothing to search.
  if (SlotI == SlotE)
    return false;

  // Start our search at the first segment that ends after the first slot.
  const_iterator SegmentI = find(*SlotI);
  const_iterator SegmentE = end();

  // If there are no segments that end after the first slot, we're done.
  if (SegmentI == SegmentE)
    return false;

  // Look for each slot in the live range.
  for ( ; SlotI != SlotE; ++SlotI) {
    // Go to the next segment that ends after the current slot.
    // The slot may be within a hole in the range.
    SegmentI = advanceTo(SegmentI, *SlotI);
    if (SegmentI == SegmentE)
      return false;

    // If this segment contains the slot, we're done.
    if (SegmentI->contains(*SlotI))
      return true;
    // Otherwise, look for the next slot.
  }

  // We didn't find a segment containing any of the slots.
  return false;
}

void LiveInterval::freeSubRange(SubRange *S) {
  S->~SubRange();
  // Memory was allocated with BumpPtr allocator and is not freed here.
}

void LiveInterval::removeEmptySubRanges() {
  SubRange **NextPtr = &SubRanges;
  SubRange *I = *NextPtr;
  while (I != nullptr) {
    if (!I->empty()) {
      NextPtr = &I->Next;
      I = *NextPtr;
      continue;
    }
    // Skip empty subranges until we find the first nonempty one.
    do {
      SubRange *Next = I->Next;
      freeSubRange(I);
      I = Next;
    } while (I != nullptr && I->empty());
    *NextPtr = I;
  }
}

void LiveInterval::clearSubRanges() {
  for (SubRange *I = SubRanges, *Next; I != nullptr; I = Next) {
    Next = I->Next;
    freeSubRange(I);
  }
  SubRanges = nullptr;
}

unsigned LiveInterval::getSize() const {
  unsigned Sum = 0;
  for (const Segment &S : segments)
    Sum += S.start.distance(S.end);
  return Sum;
}

raw_ostream& llvm::operator<<(raw_ostream& os, const LiveRange::Segment &S) {
  return os << '[' << S.start << ',' << S.end << ':' << S.valno->id << ")";
}

#if !defined(NDEBUG) || defined(LLVM_ENABLE_DUMP)
LLVM_DUMP_METHOD void LiveRange::Segment::dump() const {
  dbgs() << *this << "\n";
}
#endif

void LiveRange::print(raw_ostream &OS) const {
  if (empty())
    OS << "EMPTY";
  else {
    for (const Segment &S : segments) {
      OS << S;
      assert(S.valno == getValNumInfo(S.valno->id) && "Bad VNInfo");
    }
  }

  // Print value number info.
  if (getNumValNums()) {
    OS << "  ";
    unsigned vnum = 0;
    for (const_vni_iterator i = vni_begin(), e = vni_end(); i != e;
         ++i, ++vnum) {
      const VNInfo *vni = *i;
      if (vnum) OS << " ";
      OS << vnum << "@";
      if (vni->isUnused()) {
        OS << "x";
      } else {
        OS << vni->def;
        if (vni->isPHIDef())
          OS << "-phi";
      }
    }
  }
}

void LiveInterval::print(raw_ostream &OS) const {
  OS << PrintReg(reg) << ' ';
  super::print(OS);
  // Print subranges
  for (const SubRange &SR : subranges()) {
    OS << " L" << PrintLaneMask(SR.LaneMask) << ' ' << SR;
  }
}

#if !defined(NDEBUG) || defined(LLVM_ENABLE_DUMP)
LLVM_DUMP_METHOD void LiveRange::dump() const {
  dbgs() << *this << "\n";
}

LLVM_DUMP_METHOD void LiveInterval::dump() const {
  dbgs() << *this << "\n";
}
#endif

#ifndef NDEBUG
void LiveRange::verify() const {
  for (const_iterator I = begin(), E = end(); I != E; ++I) {
    assert(I->start.isValid());
    assert(I->end.isValid());
    assert(I->start < I->end);
    assert(I->valno != nullptr);
    assert(I->valno->id < valnos.size());
    assert(I->valno == valnos[I->valno->id]);
    if (std::next(I) != E) {
      assert(I->end <= std::next(I)->start);
      if (I->end == std::next(I)->start)
        assert(I->valno != std::next(I)->valno);
    }
  }
}

void LiveInterval::verify(const MachineRegisterInfo *MRI) const {
  super::verify();

  // Make sure SubRanges are fine and LaneMasks are disjunct.
  LaneBitmask Mask = 0;
  LaneBitmask MaxMask = MRI != nullptr ? MRI->getMaxLaneMaskForVReg(reg) : ~0u;
  for (const SubRange &SR : subranges()) {
    // Subrange lanemask should be disjunct to any previous subrange masks.
    assert((Mask & SR.LaneMask) == 0);
    Mask |= SR.LaneMask;

    // subrange mask should not contained in maximum lane mask for the vreg.
    assert((Mask & ~MaxMask) == 0);
    // empty subranges must be removed.
    assert(!SR.empty());

    SR.verify();
    // Main liverange should cover subrange.
    assert(covers(SR));
  }
}
#endif


//===----------------------------------------------------------------------===//
//                           LiveRangeUpdater class
//===----------------------------------------------------------------------===//
//
// The LiveRangeUpdater class always maintains these invariants:
//
// - When LastStart is invalid, Spills is empty and the iterators are invalid.
//   This is the initial state, and the state created by flush().
//   In this state, isDirty() returns false.
//
// Otherwise, segments are kept in three separate areas:
//
// 1. [begin; WriteI) at the front of LR.
// 2. [ReadI; end) at the back of LR.
// 3. Spills.
//
// - LR.begin() <= WriteI <= ReadI <= LR.end().
// - Segments in all three areas are fully ordered and coalesced.
// - Segments in area 1 precede and can't coalesce with segments in area 2.
// - Segments in Spills precede and can't coalesce with segments in area 2.
// - No coalescing is possible between segments in Spills and segments in area
//   1, and there are no overlapping segments.
//
// The segments in Spills are not ordered with respect to the segments in area
// 1. They need to be merged.
//
// When they exist, Spills.back().start <= LastStart,
//                 and WriteI[-1].start <= LastStart.

void LiveRangeUpdater::print(raw_ostream &OS) const {
  if (!isDirty()) {
    if (LR)
      OS << "Clean updater: " << *LR << '\n';
    else
      OS << "Null updater.\n";
    return;
  }
  assert(LR && "Can't have null LR in dirty updater.");
  OS << " updater with gap = " << (ReadI - WriteI)
     << ", last start = " << LastStart
     << ":\n  Area 1:";
  for (const auto &S : make_range(LR->begin(), WriteI))
    OS << ' ' << S;
  OS << "\n  Spills:";
  for (unsigned I = 0, E = Spills.size(); I != E; ++I)
    OS << ' ' << Spills[I];
  OS << "\n  Area 2:";
  for (const auto &S : make_range(ReadI, LR->end()))
    OS << ' ' << S;
  OS << '\n';
}

LLVM_DUMP_METHOD void LiveRangeUpdater::dump() const {
  print(errs());
}

// Determine if A and B should be coalesced.
static inline bool coalescable(const LiveRange::Segment &A,
                               const LiveRange::Segment &B) {
  assert(A.start <= B.start && "Unordered live segments.");
  if (A.end == B.start)
    return A.valno == B.valno;
  if (A.end < B.start)
    return false;
  assert(A.valno == B.valno && "Cannot overlap different values");
  return true;
}

void LiveRangeUpdater::add(LiveRange::Segment Seg) {
  assert(LR && "Cannot add to a null destination");

  // Fall back to the regular add method if the live range
  // is using the segment set instead of the segment vector.
  if (LR->segmentSet != nullptr) {
    LR->addSegmentToSet(Seg);
    return;
  }

  // Flush the state if Start moves backwards.
  if (!LastStart.isValid() || LastStart > Seg.start) {
    if (isDirty())
      flush();
    // This brings us to an uninitialized state. Reinitialize.
    assert(Spills.empty() && "Leftover spilled segments");
    WriteI = ReadI = LR->begin();
  }

  // Remember start for next time.
  LastStart = Seg.start;

  // Advance ReadI until it ends after Seg.start.
  LiveRange::iterator E = LR->end();
  if (ReadI != E && ReadI->end <= Seg.start) {
    // First try to close the gap between WriteI and ReadI with spills.
    if (ReadI != WriteI)
      mergeSpills();
    // Then advance ReadI.
    if (ReadI == WriteI)
      ReadI = WriteI = LR->find(Seg.start);
    else
      while (ReadI != E && ReadI->end <= Seg.start)
        *WriteI++ = *ReadI++;
  }

  assert(ReadI == E || ReadI->end > Seg.start);

  // Check if the ReadI segment begins early.
  if (ReadI != E && ReadI->start <= Seg.start) {
    assert(ReadI->valno == Seg.valno && "Cannot overlap different values");
    // Bail if Seg is completely contained in ReadI.
    if (ReadI->end >= Seg.end)
      return;
    // Coalesce into Seg.
    Seg.start = ReadI->start;
    ++ReadI;
  }

  // Coalesce as much as possible from ReadI into Seg.
  while (ReadI != E && coalescable(Seg, *ReadI)) {
    Seg.end = std::max(Seg.end, ReadI->end);
    ++ReadI;
  }

  // Try coalescing Spills.back() into Seg.
  if (!Spills.empty() && coalescable(Spills.back(), Seg)) {
    Seg.start = Spills.back().start;
    Seg.end = std::max(Spills.back().end, Seg.end);
    Spills.pop_back();
  }

  // Try coalescing Seg into WriteI[-1].
  if (WriteI != LR->begin() && coalescable(WriteI[-1], Seg)) {
    WriteI[-1].end = std::max(WriteI[-1].end, Seg.end);
    return;
  }

  // Seg doesn't coalesce with anything, and needs to be inserted somewhere.
  if (WriteI != ReadI) {
    *WriteI++ = Seg;
    return;
  }

  // Finally, append to LR or Spills.
  if (WriteI == E) {
    LR->segments.push_back(Seg);
    WriteI = ReadI = LR->end();
  } else
    Spills.push_back(Seg);
}

// Merge as many spilled segments as possible into the gap between WriteI
// and ReadI. Advance WriteI to reflect the inserted instructions.
void LiveRangeUpdater::mergeSpills() {
  // Perform a backwards merge of Spills and [SpillI;WriteI).
  size_t GapSize = ReadI - WriteI;
  size_t NumMoved = std::min(Spills.size(), GapSize);
  LiveRange::iterator Src = WriteI;
  LiveRange::iterator Dst = Src + NumMoved;
  LiveRange::iterator SpillSrc = Spills.end();
  LiveRange::iterator B = LR->begin();

  // This is the new WriteI position after merging spills.
  WriteI = Dst;

  // Now merge Src and Spills backwards.
  while (Src != Dst) {
    if (Src != B && Src[-1].start > SpillSrc[-1].start)
      *--Dst = *--Src;
    else
      *--Dst = *--SpillSrc;
  }
  assert(NumMoved == size_t(Spills.end() - SpillSrc));
  Spills.erase(SpillSrc, Spills.end());
}

void LiveRangeUpdater::flush() {
  if (!isDirty())
    return;
  // Clear the dirty state.
  LastStart = SlotIndex();

  assert(LR && "Cannot add to a null destination");

  // Nothing to merge?
  if (Spills.empty()) {
    LR->segments.erase(WriteI, ReadI);
    LR->verify();
    return;
  }

  // Resize the WriteI - ReadI gap to match Spills.
  size_t GapSize = ReadI - WriteI;
  if (GapSize < Spills.size()) {
    // The gap is too small. Make some room.
    size_t WritePos = WriteI - LR->begin();
    LR->segments.insert(ReadI, Spills.size() - GapSize, LiveRange::Segment());
    // This also invalidated ReadI, but it is recomputed below.
    WriteI = LR->begin() + WritePos;
  } else {
    // Shrink the gap if necessary.
    LR->segments.erase(WriteI + Spills.size(), ReadI);
  }
  ReadI = WriteI + Spills.size();
  mergeSpills();
  LR->verify();
}

unsigned ConnectedVNInfoEqClasses::Classify(const LiveRange &LR) {
  // Create initial equivalence classes.
  EqClass.clear();
  EqClass.grow(LR.getNumValNums());

  const VNInfo *used = nullptr, *unused = nullptr;

  // Determine connections.
  for (const VNInfo *VNI : LR.valnos) {
    // Group all unused values into one class.
    if (VNI->isUnused()) {
      if (unused)
        EqClass.join(unused->id, VNI->id);
      unused = VNI;
      continue;
    }
    used = VNI;
    if (VNI->isPHIDef()) {
      const MachineBasicBlock *MBB = LIS.getMBBFromIndex(VNI->def);
      assert(MBB && "Phi-def has no defining MBB");
      // Connect to values live out of predecessors.
      for (MachineBasicBlock::const_pred_iterator PI = MBB->pred_begin(),
           PE = MBB->pred_end(); PI != PE; ++PI)
        if (const VNInfo *PVNI = LR.getVNInfoBefore(LIS.getMBBEndIdx(*PI)))
          EqClass.join(VNI->id, PVNI->id);
    } else {
      // Normal value defined by an instruction. Check for two-addr redef.
      // FIXME: This could be coincidental. Should we really check for a tied
      // operand constraint?
      // Note that VNI->def may be a use slot for an early clobber def.
      if (const VNInfo *UVNI = LR.getVNInfoBefore(VNI->def))
        EqClass.join(VNI->id, UVNI->id);
    }
  }

  // Lump all the unused values in with the last used value.
  if (used && unused)
    EqClass.join(used->id, unused->id);

  EqClass.compress();
  return EqClass.getNumClasses();
}

template<typename LiveRangeT, typename EqClassesT>
static void DistributeRange(LiveRangeT &LR, LiveRangeT *SplitLRs[],
                            EqClassesT VNIClasses) {
  // Move segments to new intervals.
  LiveRange::iterator J = LR.begin(), E = LR.end();
  while (J != E && VNIClasses[J->valno->id] == 0)
    ++J;
  for (LiveRange::iterator I = J; I != E; ++I) {
    if (unsigned eq = VNIClasses[I->valno->id]) {
      assert((SplitLRs[eq-1]->empty() || SplitLRs[eq-1]->expiredAt(I->start)) &&
             "New intervals should be empty");
      SplitLRs[eq-1]->segments.push_back(*I);
    } else
      *J++ = *I;
  }
  LR.segments.erase(J, E);

  // Transfer VNInfos to their new owners and renumber them.
  unsigned j = 0, e = LR.getNumValNums();
  while (j != e && VNIClasses[j] == 0)
    ++j;
  for (unsigned i = j; i != e; ++i) {
    VNInfo *VNI = LR.getValNumInfo(i);
    if (unsigned eq = VNIClasses[i]) {
      VNI->id = SplitLRs[eq-1]->getNumValNums();
      SplitLRs[eq-1]->valnos.push_back(VNI);
    } else {
      VNI->id = j;
      LR.valnos[j++] = VNI;
    }
  }
  LR.valnos.resize(j);
}

void ConnectedVNInfoEqClasses::Distribute(LiveInterval &LI, LiveInterval *LIV[],
                                          MachineRegisterInfo &MRI) {
  // Rewrite instructions.
  for (MachineRegisterInfo::reg_iterator RI = MRI.reg_begin(LI.reg),
       RE = MRI.reg_end(); RI != RE;) {
    MachineOperand &MO = *RI;
    MachineInstr *MI = RI->getParent();
    ++RI;
    // DBG_VALUE instructions don't have slot indexes, so get the index of the
    // instruction before them.
    // Normally, DBG_VALUE instructions are removed before this function is
    // called, but it is not a requirement.
    SlotIndex Idx;
    if (MI->isDebugValue())
      Idx = LIS.getSlotIndexes()->getIndexBefore(*MI);
    else
      Idx = LIS.getInstructionIndex(*MI);
    LiveQueryResult LRQ = LI.Query(Idx);
    const VNInfo *VNI = MO.readsReg() ? LRQ.valueIn() : LRQ.valueDefined();
    // In the case of an <undef> use that isn't tied to any def, VNI will be
    // NULL. If the use is tied to a def, VNI will be the defined value.
    if (!VNI)
      continue;
    if (unsigned EqClass = getEqClass(VNI))
      MO.setReg(LIV[EqClass-1]->reg);
  }

  // Distribute subregister liveranges.
  if (LI.hasSubRanges()) {
    unsigned NumComponents = EqClass.getNumClasses();
    SmallVector<unsigned, 8> VNIMapping;
    SmallVector<LiveInterval::SubRange*, 8> SubRanges;
    BumpPtrAllocator &Allocator = LIS.getVNInfoAllocator();
    for (LiveInterval::SubRange &SR : LI.subranges()) {
      // Create new subranges in the split intervals and construct a mapping
      // for the VNInfos in the subrange.
      unsigned NumValNos = SR.valnos.size();
      VNIMapping.clear();
      VNIMapping.reserve(NumValNos);
      SubRanges.clear();
      SubRanges.resize(NumComponents-1, nullptr);
      for (unsigned I = 0; I < NumValNos; ++I) {
        const VNInfo &VNI = *SR.valnos[I];
        unsigned ComponentNum;
        if (VNI.isUnused()) {
          ComponentNum = 0;
        } else {
          const VNInfo *MainRangeVNI = LI.getVNInfoAt(VNI.def);
          assert(MainRangeVNI != nullptr
                 && "SubRange def must have corresponding main range def");
          ComponentNum = getEqClass(MainRangeVNI);
          if (ComponentNum > 0 && SubRanges[ComponentNum-1] == nullptr) {
            SubRanges[ComponentNum-1]
              = LIV[ComponentNum-1]->createSubRange(Allocator, SR.LaneMask);
          }
        }
        VNIMapping.push_back(ComponentNum);
      }
      DistributeRange(SR, SubRanges.data(), VNIMapping);
    }
    LI.removeEmptySubRanges();
  }

  // Distribute main liverange.
  DistributeRange(LI, LIV, EqClass);
}

void ConnectedSubRegClasses::renameComponents(LiveInterval &LI) const {
  // Shortcut: We cannot have split components with a single definition.
  if (LI.valnos.size() < 2)
    return;

  SmallVector<SubRangeInfo, 4> SubRangeInfos;
  IntEqClasses Classes;
  if (!findComponents(Classes, SubRangeInfos, LI))
    return;

  // Create a new VReg for each class.
  unsigned Reg = LI.reg;
  const TargetRegisterClass *RegClass = MRI.getRegClass(Reg);
  SmallVector<LiveInterval*, 4> Intervals;
  Intervals.push_back(&LI);
  for (unsigned I = 1, NumClasses = Classes.getNumClasses(); I < NumClasses;
       ++I) {
    unsigned NewVReg = MRI.createVirtualRegister(RegClass);
    LiveInterval &NewLI = LIS.createEmptyInterval(NewVReg);
    Intervals.push_back(&NewLI);
  }

  rewriteOperands(Classes, SubRangeInfos, Intervals);
  distribute(Classes, SubRangeInfos, Intervals);
  computeMainRangesFixFlags(Classes, SubRangeInfos, Intervals);
}

bool ConnectedSubRegClasses::findComponents(IntEqClasses &Classes,
    SmallVectorImpl<ConnectedSubRegClasses::SubRangeInfo> &SubRangeInfos,
    LiveInterval &LI) const {
  // First step: Create connected components for the VNInfos inside the
  // subranges and count the global number of such components.
  unsigned NumComponents = 0;
  for (LiveInterval::SubRange &SR : LI.subranges()) {
    SubRangeInfos.push_back(SubRangeInfo(LIS, SR, NumComponents));
    ConnectedVNInfoEqClasses &ConEQ = SubRangeInfos.back().ConEQ;

    unsigned NumSubComponents = ConEQ.Classify(SR);
    NumComponents += NumSubComponents;
  }
  // Shortcut: With only 1 subrange, the normal separate component tests are
  // enough and we do not need to perform the union-find on the subregister
  // segments.
  if (SubRangeInfos.size() < 2)
    return false;

  // Next step: Build union-find structure over all subranges and merge classes
  // across subranges when they are affected by the same MachineOperand.
  const TargetRegisterInfo &TRI = *MRI.getTargetRegisterInfo();
  Classes.grow(NumComponents);
  unsigned Reg = LI.reg;
  for (const MachineOperand &MO : MRI.reg_nodbg_operands(Reg)) {
    if (!MO.isDef() && !MO.readsReg())
      continue;
    unsigned SubRegIdx = MO.getSubReg();
    LaneBitmask LaneMask = TRI.getSubRegIndexLaneMask(SubRegIdx);
    unsigned MergedID = ~0u;
    for (ConnectedSubRegClasses::SubRangeInfo &SRInfo : SubRangeInfos) {
      const LiveInterval::SubRange &SR = *SRInfo.SR;
      if ((SR.LaneMask & LaneMask) == 0)
        continue;
      SlotIndex Pos = LIS.getInstructionIndex(*MO.getParent());
      Pos = MO.isDef() ? Pos.getRegSlot(MO.isEarlyClobber())
                       : Pos.getBaseIndex();
      const VNInfo *VNI = SR.getVNInfoAt(Pos);
      if (VNI == nullptr)
        continue;

      // Map to local representant ID.
      unsigned LocalID = SRInfo.ConEQ.getEqClass(VNI);
      // Global ID
      unsigned ID = LocalID + SRInfo.Index;
      // Merge other sets
      MergedID = MergedID == ~0u ? ID : Classes.join(MergedID, ID);
    }
  }

  // Early exit if we ended up with a single equivalence class.
  Classes.compress();
  unsigned NumClasses = Classes.getNumClasses();
  return NumClasses > 1;
}

void ConnectedSubRegClasses::rewriteOperands(const IntEqClasses &Classes,
    const SmallVectorImpl<SubRangeInfo> &SubRangeInfos,
    const SmallVectorImpl<LiveInterval*> &Intervals) const {
  const TargetRegisterInfo &TRI = *MRI.getTargetRegisterInfo();
  unsigned Reg = Intervals[0]->reg;;
  for (MachineRegisterInfo::reg_nodbg_iterator I = MRI.reg_nodbg_begin(Reg),
       E = MRI.reg_nodbg_end(); I != E; ) {
    MachineOperand &MO = *I++;
    if (!MO.isDef() && !MO.readsReg())
      continue;

    MachineInstr &MI = *MO.getParent();

    SlotIndex Pos = LIS.getInstructionIndex(MI);
    unsigned SubRegIdx = MO.getSubReg();
    LaneBitmask LaneMask = TRI.getSubRegIndexLaneMask(SubRegIdx);

    unsigned ID = ~0u;
    for (const SubRangeInfo &SRInfo : SubRangeInfos) {
      const LiveInterval::SubRange &SR = *SRInfo.SR;
      if ((SR.LaneMask & LaneMask) == 0)
        continue;
      LiveRange::const_iterator I = SR.find(Pos);
      if (I == SR.end())
        continue;

      const VNInfo &VNI = *I->valno;
      // Map to local representant ID.
      unsigned LocalID = SRInfo.ConEQ.getEqClass(&VNI);
      // Global ID
      ID = Classes[LocalID + SRInfo.Index];
      break;
    }

    unsigned VReg = Intervals[ID]->reg;
    MO.setReg(VReg);
  }
}

void ConnectedSubRegClasses::distribute(const IntEqClasses &Classes,
    const SmallVectorImpl<SubRangeInfo> &SubRangeInfos,
    const SmallVectorImpl<LiveInterval*> &Intervals) const {
  unsigned NumClasses = Classes.getNumClasses();
  SmallVector<unsigned, 8> VNIMapping;
  SmallVector<LiveInterval::SubRange*, 8> SubRanges;
  BumpPtrAllocator &Allocator = LIS.getVNInfoAllocator();
  for (const SubRangeInfo &SRInfo : SubRangeInfos) {
    LiveInterval::SubRange &SR = *SRInfo.SR;
    unsigned NumValNos = SR.valnos.size();
    VNIMapping.clear();
    VNIMapping.reserve(NumValNos);
    SubRanges.clear();
    SubRanges.resize(NumClasses-1, nullptr);
    for (unsigned I = 0; I < NumValNos; ++I) {
      const VNInfo &VNI = *SR.valnos[I];
      unsigned LocalID = SRInfo.ConEQ.getEqClass(&VNI);
      unsigned ID = Classes[LocalID + SRInfo.Index];
      VNIMapping.push_back(ID);
      if (ID > 0 && SubRanges[ID-1] == nullptr)
        SubRanges[ID-1] = Intervals[ID]->createSubRange(Allocator, SR.LaneMask);
    }
    DistributeRange(SR, SubRanges.data(), VNIMapping);
  }
}

static bool subRangeLiveAt(const LiveInterval &LI, SlotIndex Pos) {
  for (const LiveInterval::SubRange &SR : LI.subranges()) {
    if (SR.liveAt(Pos))
      return true;
  }
  return false;
}

void ConnectedSubRegClasses::computeMainRangesFixFlags(
    const IntEqClasses &Classes,
    const SmallVectorImpl<SubRangeInfo> &SubRangeInfos,
    const SmallVectorImpl<LiveInterval*> &Intervals) const {
  BumpPtrAllocator &Allocator = LIS.getVNInfoAllocator();
  const SlotIndexes &Indexes = *LIS.getSlotIndexes();
  for (size_t I = 0, E = Intervals.size(); I < E; ++I) {
    LiveInterval &LI = *Intervals[I];
    unsigned Reg = LI.reg;

    LI.removeEmptySubRanges();

    // There must be a def (or live-in) before every use. Splitting vregs may
    // violate this principle as the splitted vreg may not have a definition on
    // every path. Fix this by creating IMPLICIT_DEF instruction as necessary.
    for (const LiveInterval::SubRange &SR : LI.subranges()) {
      // Search for "PHI" value numbers in the subranges. We must find a live
      // value in each predecessor block, add an IMPLICIT_DEF where it is
      // missing.
      for (unsigned I = 0; I < SR.valnos.size(); ++I) {
        const VNInfo &VNI = *SR.valnos[I];
        if (VNI.isUnused() || !VNI.isPHIDef())
          continue;

        SlotIndex Def = VNI.def;
        MachineBasicBlock &MBB = *Indexes.getMBBFromIndex(Def);
        for (MachineBasicBlock *PredMBB : MBB.predecessors()) {
          SlotIndex PredEnd = Indexes.getMBBEndIdx(PredMBB);
          if (subRangeLiveAt(LI, PredEnd.getPrevSlot()))
            continue;

          MachineBasicBlock::iterator InsertPos =
            llvm::findPHICopyInsertPoint(PredMBB, &MBB, Reg);
          const MCInstrDesc &MCDesc = TII.get(TargetOpcode::IMPLICIT_DEF);
          MachineInstrBuilder ImpDef = BuildMI(*PredMBB, InsertPos,
                                               DebugLoc(), MCDesc, Reg);
          SlotIndex DefIdx = LIS.InsertMachineInstrInMaps(*ImpDef);
          SlotIndex RegDefIdx = DefIdx.getRegSlot();
          for (LiveInterval::SubRange &SR : LI.subranges()) {
            VNInfo *SRVNI = SR.getNextValue(RegDefIdx, Allocator);
            SR.addSegment(LiveRange::Segment(RegDefIdx, PredEnd, SRVNI));
          }
        }
      }
    }

    for (MachineOperand &MO : MRI.reg_nodbg_operands(Reg)) {
      if (!MO.isDef())
        continue;
      unsigned SubRegIdx = MO.getSubReg();
      if (SubRegIdx == 0)
        continue;
      // After assigning the new vreg we may not have any other sublanes living
      // in and out of the instruction anymore. We need to add new dead and
      // undef flags in these cases.
      if (!MO.isUndef()) {
        SlotIndex Pos = LIS.getInstructionIndex(*MO.getParent());
        if (!subRangeLiveAt(LI, Pos))
          MO.setIsUndef();
      }
      if (!MO.isDead()) {
        SlotIndex Pos = LIS.getInstructionIndex(*MO.getParent()).getDeadSlot();
        if (!subRangeLiveAt(LI, Pos))
          MO.setIsDead();
      }
    }

    if (I == 0)
      LI.clear();
    LIS.constructMainRangeFromSubranges(LI);
  }
}
