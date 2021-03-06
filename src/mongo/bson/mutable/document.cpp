/* Copyright 2013 10gen Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "mongo/platform/basic.h"

#include "mongo/bson/mutable/document.h"

#include <boost/scoped_ptr.hpp>
#include <boost/static_assert.hpp>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <vector>

#include "mongo/bson/inline_decls.h"

#include "mongo/bson/mutable/damage_vector.h"

namespace mongo {
namespace mutablebson {

    /** Mutable BSON Implementation Overview
     *
     *  If you haven't read it already, please read the 'Mutable BSON Overview' comment in
     *  document.h before reading further.
     *
     *  In the following discussion, the capitalized terms 'Element' and 'Document' refer to
     *  the classes of the same name. At times, it is also necessary to refer to abstract
     *  'elements' or 'documents', in the sense of bsonspec.org. These latter uses are
     *  non-capitalized. In the BSON specification, there are two 'classes' of
     *  elements. 'Primitive' or 'leaf' elements are those elements which do not contain other
     *  elements. In practice, all BSON types except 'Array' and 'Object' are primitives. The
     *  CodeWScope type is an exception, but one that we sidestep by considering its BSONObj
     *  payload to be opaque.
     *
     *  A mutable BSON Document and its component Elements are implemented in terms of four
     *  data structures. These structures are owned by a Document::Impl object. Each Document
     *  owns a unique Document::Impl, which owns the relevant data structures and provides
     *  accessors, mutators, and helper methods related to those data structures. Understanding
     *  these data structures is critical for understanding how the system as a whole operates.
     *
     *  - The 'Elements Vector': This is a std::vector<ElementRep>, where 'ElementRep' is a
     *    structure type defined below that contains the detailed information about an entity
     *    in the Document (e.g. an Object, or an Array, or a NumberLong, etc.). The 'Element'
     *    and 'ConstElement' objects contain a pointer to a Document (which allows us to reach
     *    the Document::Impl for the Document), and an index into the Elements Vector in the
     *    Document::Impl. These two pieces of information make it possible for us to obtain the
     *    ElementRep associated with a given Element. Note that the Elements Vector is append
     *    only: ElementReps are never removed from it, even if the cooresponding Element is
     *    removed from the Document. By never removing ElementReps, and by using indexes into
     *    the Elements Vector, we can ensure that Elements are never invalidated. Note that
     *    every Document comes with an automatically provided 'root' element of mongo::Object
     *    type. The ElementRep for the root is always in the first slot (index zero) of the
     *    Elements Vector.
     *
     *  - The 'Leaf Builder': This is a standard BSONObjBuilder. When a request is made to the
     *    Document to add new data to the Document via one of the Document::makeElement[TYPE]
     *    calls, the element is constructed by invoking the appropriate method on the Leaf
     *    Builder, forwarding the arguments provided to the call on Document. This results in a
     *    contiguous region of memory which encodes this element, capturing its field name, its
     *    type, and the bytes that encode its value, in the same way it normally does when
     *    using BSONObjBuilder. We then build an ElementRep that indexes into the BufBuilder
     *    behind the BSONObjBuilder (more on how this happens below, in the section on the
     *    'Objects Vector'), then insert that new ElementRep into the ElementsVector, and
     *    finally return an Element that dereferences to the new ElementRep. Subsequently,
     *    requests for the type, fieldname or value bytes via the Element are satisfied by
     *    obtaining the contiguous memory region for the element, which may be used to
     *    construct a BSONElement over that memory region.
     *
     *  - The 'Objects Vector': This is a std::vector<BSONObj>. Any BSONObj object that
     *    provides values for parts of the Document is stored in the Objects Vector. For
     *    instance, in 'Example 2' from document.h, the Document we construct wraps an existing
     *    BSONObj, which is passed in to the Document constructor. That BSONObj would be stored
     *    in the Objects Vector. The data content of the BSONObj is not copied, but the BSONObj
     *    is copied, so the if the BSONObj is counted, we will up its refcount. In any event
     *    the lifetime of the BSONObj must exceed our lifetime by some mechanism. ElementReps
     *    that represent the component elements of the BSONObj store the index of their
     *    supporting BSONObj into the 'objIdx' field of ElementRep. Later, when Elements
     *    referring to those ElementReps are asked for properties like the field name or type
     *    of the Element, the underlying memory region in the appropriate BSONObj may be
     *    examined to provide the relevant data.
     *
     *  - The 'Field Name Heap': For some elements, particularly those in the Leaf Builder or
     *    those embedded in a BSONObj in the Objects Vector, we can easily obtain the field
     *    name by reading it from the encoded BSON. However, some elements are not so
     *    fortunate. Newly created elements of mongo::Array or mongo::Object type, for
     *    instance, don't have a memory region that provides values. In such cases, the field
     *    name is stored in the field name heap, which is simply std::vector<char>, where the
     *    field names are null-byte-delimited. ElementsReps for such elements store an offset
     *    into the Field Name Heap, and when asked for their field name simply return a pointer
     *    to the string data the offset identifies. This exploits the fact that in BSON, valid
     *    field names are null terinated and do not contain embedded null bytes.
     *
     *  - The 'root' Element. Each Document contains a well known Element, which always refers
     *    to a pre-constructed ElementRep at offset zero in the Elements Vector. This is an
     *    Object element, and it is considered as the root of the document tree. It is possible
     *    for ElementReps to exist in the Document data structures, but not be in a child
     *    relationship to the root Element. Newly created Elements, for instance, are in this
     *    sort of 'detached' state until they are attched to another element. Only Element's
     *    that are children of the root element are traversed when calling top level
     *    serialization or comparision operations on Document.
     *
     *  When you construct a Document that obtains its values from an underlying BSONObj, the
     *  entire BSONObj is not 'unpacked' into ElementReps at Document construction
     *  time. Instead, as you ask for Elements with the Element navigation API, the Elements
     *  for children and siblings are created on demand. Subobjects which are never visited
     *  will never have ElementReps constructed for them. Similarly, when writing a Document
     *  back out to a builder, regions of memory that provide values for the Document and which
     *  have not been modified will be block copied, instead of being recursively explored and
     *  written.
     *
     *  To see how these data structures interoperate, we will walk through an example. You may
     *  want to read the comments for ElementRep before tackling the example, since we will
     *  refer to the internal state of ElementRep here. The example code used here exists as a
     *  unit test in mutable_bson_test.cpp as (Documentation, Example3).
     *
     *
     *  Legend:
     *   oi   : objIdx
     *   +/-  : bitfield state (s: serialized, a: array)
     *   x    : invalid/empty rep idx
     *   ?    : opaque rep idx
     *   ls/rs: left/right sibling
     *   lc/rc: left/right child
     *   p    : parent

        static const char inJson[] =
            "{"
            "  'xs': { 'x' : 'x', 'X' : 'X' },"
            "  'ys': { 'y' : 'y' }"
            "}";
        mongo::BSONObj inObj = mongo::fromjson(inJson);
        mmb::Document doc(inObj);

     *    _elements
     *      oi      flags                offset                  ls  rs  lc  rc  p
     *    +-----------------------------------------------------------------------------+
     *  0 | 1 | s:- | ...       | off 0       into _fieldNames | x | x | ? | ? | x      |
     *    +-----------------------------------------------------------------------------+
     *
     *    _objects
     *    +-----------------------------------------------------------------------------+
     *    | BSONObj for _leafBuilder | BSONObj for inObj |                              |
     *    +-----------------------------------------------------------------------------+
     *
     *    _fieldNames
     *    +-----------------------------------------------------------------------------+
     *    | \0                                                                          |
     *    +-----------------------------------------------------------------------------+
     *
     *    _leafBuf
     *    +-----------------------------------------------------------------------------+
     *    | {}                                                                          |
     *    +-----------------------------------------------------------------------------+


        mmb::Element root = doc.root();
        mmb::Element xs = root.leftChild();

     *    _elements
     *      oi      flags                offset                  ls  rs  lc  rc  p
     *    +-----------------------------------------------------------------------------+
     *  0 | 1 | s:- | ...       | off 0       into _fieldNames | x | x | 1 | ? | x      | *
     *  1 | 1 | s:+ | ...       | off of 'xs' into _objects[1] | x | ? | ? | ? | 0      | *
     *    +-----------------------------------------------------------------------------+
     *
     *    _objects
     *    +-----------------------------------------------------------------------------+
     *    | BSONObj for _leafBuilder | BSONObj for inObj |                              |
     *    +-----------------------------------------------------------------------------+
     *
     *    _fieldNames
     *    +-----------------------------------------------------------------------------+
     *    | \0                                                                          |
     *    +-----------------------------------------------------------------------------+
     *
     *    _leafBuf
     *    +-----------------------------------------------------------------------------+
     *    | {}                                                                          |
     *    +-----------------------------------------------------------------------------+


        mmb::Element ys = xs.rightSibling();

     *    _elements
     *      oi      flags                offset                  ls  rs  lc  rc  p
     *    +-----------------------------------------------------------------------------+
     *  0 | 1 | s:- | ...       | off 0       into _fieldNames | x | x | 1 | ? | x      |
     *  1 | 1 | s:+ | ...       | off of 'xs' into _objects[1] | x | 2 | ? | ? | 0      | *
     *  2 | 1 | s:+ | ...       | off of 'ys' into _objects[1] | 1 | ? | ? | ? | 0      | *
     *    +-----------------------------------------------------------------------------+
     *
     *    _objects
     *    +-----------------------------------------------------------------------------+
     *    | BSONObj for _leafBuilder | BSONObj for inObj |                              |
     *    +-----------------------------------------------------------------------------+
     *
     *    _fieldNames
     *    +-----------------------------------------------------------------------------+
     *    | \0                                                                          |
     *    +-----------------------------------------------------------------------------+
     *
     *    _leafBuf
     *    +-----------------------------------------------------------------------------+
     *    | {}                                                                          |
     *    +-----------------------------------------------------------------------------+


        mmb::Element dne = ys.rightSibling();

     *    _elements
     *      oi      flags                offset                  ls  rs  lc  rc  p
     *    +-----------------------------------------------------------------------------+
     *  0 | 1 | s:- | ...       | off 0       into _fieldNames | x | x | 1 | 2 | x      | *
     *  1 | 1 | s:+ | ...       | off of 'xs' into _objects[1] | x | 2 | ? | ? | 0      |
     *  2 | 1 | s:+ | ...       | off of 'ys' into _objects[1] | 1 | x | ? | ? | 0      | *
     *    +-----------------------------------------------------------------------------+
     *
     *    _objects
     *    +-----------------------------------------------------------------------------+
     *    | BSONObj for _leafBuilder | BSONObj for inObj |                              |
     *    +-----------------------------------------------------------------------------+
     *
     *    _fieldNames
     *    +-----------------------------------------------------------------------------+
     *    | \0                                                                          |
     *    +-----------------------------------------------------------------------------+
     *
     *    _leafBuf
     *    +-----------------------------------------------------------------------------+
     *    | {}                                                                          |
     *    +-----------------------------------------------------------------------------+


        mmb::Element ycaps = doc.makeElementString("Y", "Y");

     *    _elements
     *      oi      flags                offset                  ls  rs  lc  rc  p
     *    +-----------------------------------------------------------------------------+
     *  0 | 1 | s:- | ...       | off 0       into _fieldNames | x | x | 1 | 2 | x      |
     *  1 | 1 | s:+ | ...       | off of 'xs' into _objects[1] | x | 2 | ? | ? | 0      |
     *  2 | 1 | s:+ | ...       | off of 'ys' into _objects[1] | 1 | x | ? | ? | 0      |
     *  3 | 0 | s:+ | ...       | off of 'Y'  into _objects[0] | x | x | x | x | x      | *
     *    +-----------------------------------------------------------------------------+
     *
     *    _objects
     *    +-----------------------------------------------------------------------------+
     *    | BSONObj for _leafBuilder | BSONObj for inObj |                              |
     *    +-----------------------------------------------------------------------------+
     *
     *    _fieldNames
     *    +-----------------------------------------------------------------------------+
     *    | \0                                                                          |
     *    +-----------------------------------------------------------------------------+
     *
     *    _leafBuf
     *    +-----------------------------------------------------------------------------+
     *    | { "Y" : "Y" }                                                               | *
     *    +-----------------------------------------------------------------------------+


        ys.pushBack(ycaps);

     *    _elements
     *      oi      flags                offset                    ls  rs  lc  rc  p
     *    +-----------------------------------------------------------------------------+
     *  0 | 1 | s:- | ...       | off 0         into _fieldNames | x | x | 1 | 2 | x    |
     *  1 | 1 | s:+ | ...       | off of 'xs'   into _objects[1] | x | 2 | ? | ? | 0    |
     *  2 | 1 | s:- | ...       | off of 'ys'   into _objects[1] | 1 | x | 4 | 3 | 0    | *
     *  3 | 0 | s:+ | ...       | off of 'Y'    into _objects[0] | 4 | x | x | x | 2    | *
     *  4 | 1 | s:+ | ...       | off of 'ys.y' into _objects[1] | x | 3 | x | x | 2    | *
     *    +-----------------------------------------------------------------------------+
     *
     *    _objects
     *    +-----------------------------------------------------------------------------+
     *    | BSONObj for _leafBuilder | BSONObj for inObj |                              |
     *    +-----------------------------------------------------------------------------+
     *
     *    _fieldNames
     *    +-----------------------------------------------------------------------------+
     *    | \0                                                                          |
     *    +-----------------------------------------------------------------------------+
     *
     *    _leafBuf
     *    +-----------------------------------------------------------------------------+
     *    | { "Y" : "Y" }                                                               |
     *    +-----------------------------------------------------------------------------+


        mmb::Element pun = doc.makeElementArray("why");

     *    _elements
     *      oi      flags                offset                     ls  rs  lc  rc  p
     *    +-----------------------------------------------------------------------------+
     *  0 | 1  | s:- | ...       | off 0         into _fieldNames | x | x | 1 | 2 | x   |
     *  1 | 1  | s:+ | ...       | off of 'xs'   into _objects[1] | x | 2 | ? | ? | 0   |
     *  2 | 1  | s:- | ...       | off of 'ys'   into _objects[1] | 1 | x | 4 | 3 | 0   |
     *  3 | 0  | s:+ | ...       | off of 'Y'    into _objects[0] | 4 | x | x | x | 2   |
     *  4 | 1  | s:+ | ...       | off of 'ys.y' into _objects[1] | x | 3 | x | x | 2   |
     *  5 | -1 | s:- | a:+ | ... | off of 'why'  into _fieldNames | x | x | x | x | x   | *
     *    +-----------------------------------------------------------------------------+
     *
     *    _objects
     *    +-----------------------------------------------------------------------------+
     *    | BSONObj for _leafBuilder | BSONObj for inObj |                              |
     *    +-----------------------------------------------------------------------------+
     *
     *    _fieldNames
     *    +-----------------------------------------------------------------------------+
     *    | \0why\0                                                                     | *
     *    +-----------------------------------------------------------------------------+
     *
     *    _leafBuf
     *    +-----------------------------------------------------------------------------+
     *    | { "Y" : "Y" }                                                               |
     *    +-----------------------------------------------------------------------------+


        ys.pushBack(pun);

     *    _elements
     *      oi      flags                offset                     ls  rs  lc  rc  p
     *    +-----------------------------------------------------------------------------+
     *  0 | 1  | s:- | ...       | off 0         into _fieldNames | x | x | 1 | 2 | x   |
     *  1 | 1  | s:+ | ...       | off of 'xs'   into _objects[1] | x | 2 | ? | ? | 0   |
     *  2 | 1  | s:- | ...       | off of 'ys'   into _objects[1] | 1 | x | 4 | 5 | 0   | *
     *  3 | 0  | s:+ | ...       | off of 'Y'    into _objects[0] | 4 | 5 | x | x | 2   | *
     *  4 | 1  | s:+ | ...       | off of 'ys.y' into _objects[1] | x | 3 | x | x | 2   |
     *  5 | -1 | s:- | a:+ | ... | off of 'why'  into _fieldNames | 3 | x | x | x | 2   | *
     *    +-----------------------------------------------------------------------------+
     *
     *    _objects
     *    +-----------------------------------------------------------------------------+
     *    | BSONObj for _leafBuilder | BSONObj for inObj |                              |
     *    +-----------------------------------------------------------------------------+
     *
     *    _fieldNames
     *    +-----------------------------------------------------------------------------+
     *    | \0why\0                                                                     |
     *    +-----------------------------------------------------------------------------+
     *
     *    _leafBuf
     *    +-----------------------------------------------------------------------------+
     *    | { "Y" : "Y" }                                                               |
     *    +-----------------------------------------------------------------------------+


        pun.appendString("na", "not");

     *    _elements
     *      oi      flags                offset                     ls  rs  lc  rc  p
     *    +-----------------------------------------------------------------------------+
     *  0 | 1  | s:- | ...       | off 0         into _fieldNames | x | x | 1 | 2 | x   |
     *  1 | 1  | s:+ | ...       | off of 'xs'   into _objects[1] | x | 2 | ? | ? | 0   |
     *  2 | 1  | s:- | ...       | off of 'ys'   into _objects[1] | 1 | x | 4 | 5 | 0   |
     *  3 | 0  | s:+ | ...       | off of 'Y'    into _objects[0] | 4 | 5 | x | x | 2   |
     *  4 | 1  | s:+ | ...       | off of 'ys.y' into _objects[1] | x | 3 | x | x | 2   |
     *  5 | -1 | s:- | a:+ | ... | off of 'why'  into _fieldNames | 3 | x | 6 | 6 | 2   | *
     *  6 | 0  | s:+ | ...       | off of 'na'   into _objects[0] | x | x | x | x | 5   | *
     *    +-----------------------------------------------------------------------------+
     *
     *    _objects
     *    +-----------------------------------------------------------------------------+
     *    | BSONObj for _leafBuilder | BSONObj for inObj |                              |
     *    +-----------------------------------------------------------------------------+
     *
     *    _fieldNames
     *    +-----------------------------------------------------------------------------+
     *    | \0why\0                                                                     |
     *    +-----------------------------------------------------------------------------+
     *
     *    _leafBuf
     *    +-----------------------------------------------------------------------------+
     *    | { "Y" : "Y", "na" : "not" }                                                 | *
     *    +-----------------------------------------------------------------------------+
     *
     */

// Work around http://gcc.gnu.org/bugzilla/show_bug.cgi?id=29365. Note that the selection of
// minor version 4 is somewhat arbitrary. It does appear that the fix for this was backported
// to earlier versions. This is a conservative choice that we can revisit later. We need the
// __clang__ here because Clang claims to be gcc of some version.
#if defined(__clang__) || !defined(__GNUC__) || (__GNUC__ > 4) || (__GNUC__ == 4 && __GNUC_MINOR__ >= 4)
    namespace {
#endif

        // The designated field name for the root element.
        const char kRootFieldName[] = "";

        // An ElementRep contains the information necessary to locate the data for an Element,
        // and the topology information for how the Element is related to other Elements in the
        // document.
#pragma pack(push, 1)
        struct ElementRep {

            // The index of the BSONObj that provides the value for this Element. For nodes
            // where serialized is 'false', this value may be kInvalidObjIdx to indicate that
            // the Element does not have a supporting BSONObj.
            typedef uint16_t ObjIdx;
            ObjIdx objIdx;

            // This bit is true if this ElementRep identifies a completely serialized
            // BSONElement (i.e. a region of memory with a bson type byte, a fieldname, and an
            // encoded value). Changes to children of a serialized element will cause it to be
            // marked as unserialized.
            uint16_t serialized: 1;

            // For object like Elements where we cannot determine the type of the object by
            // looking a region of memory, the 'array' bit allows us to determine whether we
            // are an object or an array.
            uint16_t array: 1;

            // Reserved for future use.
            uint16_t reserved: 14;

            // This word either gives the offset into the BSONObj associated with this
            // ElementRep where this serialized BSON element may be located, or the offset into
            // the _fieldNames member of the Document where the field name for this BSON
            // element may be located.
            uint32_t offset;

            // The indexes of our left and right siblings in the Document.
            struct {
                Element::RepIdx left;
                Element::RepIdx right;
            } sibling;

            // The indexes of our left and right chidren in the Document.
            struct {
                Element::RepIdx left;
                Element::RepIdx right;
            } child;

            // The index of our parent in the Document.
            Element::RepIdx parent;

            // Pad this object out to 32 bytes.
            //
            // TODO: Cache element size here?
            uint32_t pad;
        };
#pragma pack(pop)

        BOOST_STATIC_ASSERT(sizeof(ElementRep) == 32);

        // We want ElementRep to be a POD so Document::Impl can grow the std::vector with
        // memmove.
        //
        // TODO: C++11 static_assert(std::is_pod<ElementRep>::value);

        // The ElementRep for the root element is always zero.
        const Element::RepIdx kRootRepIdx = Element::RepIdx(0);

        // A rep for entries that do not exist (this was 'x' in the example legend).
        const Element::RepIdx kInvalidRepIdx = Element::RepIdx(-1);

        // A rep that points to an unexamined entity (this was '?' in the example legend).
        const Element::RepIdx kOpaqueRepIdx = Element::RepIdx(-2);

        // This is the highest valid rep that does not overlap flag values.
        const Element::RepIdx kMaxRepIdx = Element::RepIdx(-3);

        // This is the object index for elements in the leaf heap.
        const ElementRep::ObjIdx kLeafObjIdx = ElementRep::ObjIdx(0);

        // This is the sentinel value to indicate that we have no supporting BSONObj.
        const ElementRep::ObjIdx kInvalidObjIdx = ElementRep::ObjIdx(-1);

        // This is the highest valid object index that does not overlap sentinel values.
        const ElementRep::ObjIdx kMaxObjIdx = ElementRep::ObjIdx(-2);

        // Returns the offset of 'elt' within 'object' as a uint32_t. The element must be part
        // of the object or the behavior is undefined.
        uint32_t getElementOffset(const BSONObj& object, const BSONElement& elt) {
            dassert(!elt.eoo());
            const char* const objRaw = object.objdata();
            const char* const eltRaw = elt.rawdata();
            dassert(objRaw < eltRaw);
            dassert(eltRaw < objRaw + object.objsize());
            dassert(eltRaw + elt.size() <= objRaw + object.objsize());
            const ptrdiff_t offset = eltRaw - objRaw;
            // BSON documents express their size as an int32_t so we should always be able to
            // express the offset as a uint32_t.
            verify(offset > 0);
            verify(offset <= std::numeric_limits<int32_t>::max());
            return offset;
        }

        // For ElementRep to be a POD it can't have a constructor, so this will have to do.
        ElementRep makeRep() {
            ElementRep rep;
            rep.objIdx = kInvalidObjIdx;
            rep.serialized = false;
            rep.array = false;
            rep.reserved = 0;
            rep.offset = 0;
            rep.sibling.left = kInvalidRepIdx;
            rep.sibling.right = kInvalidRepIdx;
            rep.child.left = kInvalidRepIdx;
            rep.child.right = kInvalidRepIdx;
            rep.parent = kInvalidRepIdx;
            rep.pad = 0;
            return rep;
        }

        // Returns true if this ElementRep is 'detached' from all other elements and can be
        // added as a child, which helps ensure that we maintain a tree rather than a graph
        // when adding new elements to the tree. The root element is never considered to be
        // attachable.
        bool canAttach(const Element::RepIdx id, const ElementRep& rep) {
            return
                (id != kRootRepIdx) &&
                (rep.sibling.left == kInvalidRepIdx) &&
                (rep.sibling.right == kInvalidRepIdx) &&
                (rep.parent == kInvalidRepIdx);
        }

        // Returns a Status describing why 'canAttach' returned false. This function should not
        // be inlined since it just makes the callers larger for no real gain.
        NOINLINE_DECL Status getAttachmentError(const ElementRep& rep);
        Status getAttachmentError(const ElementRep& rep) {
            if (rep.sibling.left != kInvalidRepIdx)
                return Status(ErrorCodes::IllegalOperation, "dangling left sibling");
            if (rep.sibling.right != kInvalidRepIdx)
                return Status(ErrorCodes::IllegalOperation, "dangling right sibling");
            if (rep.parent != kInvalidRepIdx)
                return Status(ErrorCodes::IllegalOperation, "dangling parent");
            return Status(ErrorCodes::IllegalOperation, "cannot add the root as a child");
        }


        // Enable paranoid mode to force a reallocation on mutation of the princple data
        // structures in Document::Impl. This is really slow, but can be very helpful if you
        // suspect an invalidation logic error and want to find it with valgrind. Paranoid mode
        // only works in debug mode; it is ignored in release builds.
        const bool paranoid = false;

#if defined(__clang__) || !defined(__GNUC__) || (__GNUC__ > 4) || (__GNUC__ == 4 && __GNUC_MINOR__ >= 4)
    } // namespace
#endif

    /** Document::Impl holds the Document state. Please see the file comment above for details
     *  on the fields of Impl and how they are used to realize the implementation of mutable
     *  BSON. Impl provides various utility methods to insert, lookup, and interrogate the
     *  Elements, BSONObj objects, field names, and builders associated with the Document.
     *
     *  TODO: At some point, we could remove the firewall and inline the members of Impl into
     *  Document.
     */
    class Document::Impl {
        MONGO_DISALLOW_COPYING(Impl);

    public:
        Impl(Document::InPlaceMode inPlaceMode)
            : _elements()
            , _objects()
            , _fieldNames()
            , _leafBuf()
            , _leafBuilder(_leafBuf)
            , _damages() {
            // We always have a BSONObj for the leaves, so reserve one.
            _objects.reserve(1);
            // We need an object at _objects[0] so that we can access leaf elements we
            // construct with the leaf builder in the same way we access elements serialized in
            // other BSONObjs. So we call asTempObj on the builder and store the result in slot
            // 0.
            dassert(_objects.size() == kLeafObjIdx);
            _objects.push_back(_leafBuilder.asTempObj());

            // TODO: Could use boost optional to reduce pointer chasing.
            if (inPlaceMode == Document::kInPlaceEnabled) {
                _damages.reset(new DamageVector);
            }

        }

        // Obtain the ElementRep for the given rep id.
        ElementRep& getElementRep(Element::RepIdx id) {
            dassert(id < _elements.size());
            return _elements[id];
        }

        // Obtain the ElementRep for the given rep id.
        const ElementRep& getElementRep(Element::RepIdx id) const {
            dassert(id < _elements.size());
            return _elements[id];
        }

        // Insert the given ElementRep and return an ID for it.
        Element::RepIdx insertElement(const ElementRep& rep) {
            const Element::RepIdx id = _elements.size();
            verify(id <= kMaxRepIdx);
            _elements.push_back(rep);
            if (debug && paranoid) {
                // Force all reps to new addresses to help catch invalid rep usage.
                std::vector<ElementRep> new_elements(_elements);
                _elements.swap(new_elements);
            }
            return id;
        }

        // Insert a new ElementRep for a leaf element at the given offset and return its ID.
        Element::RepIdx insertLeafElement(int offset) {
            // BufBuilder hands back sizes in 'int's.
            ElementRep rep = makeRep();
            rep.objIdx = kLeafObjIdx;
            rep.serialized = true;
            dassert(offset >= 0);
            // TODO: Is this a legitimate possibility?
            dassert(static_cast<unsigned int>(offset) < std::numeric_limits<uint32_t>::max());
            rep.offset = offset;
            _objects[kLeafObjIdx] = _leafBuilder.asTempObj();
            return insertElement(rep);
        }

        // Obtain the object builder for the leaves.
        BSONObjBuilder& leafBuilder() {
            return _leafBuilder;
        }

        // Obtain the BSONObj for the given object id.
        BSONObj& getObject(ElementRep::ObjIdx objIdx) {
            dassert(objIdx < _objects.size());
            return _objects[objIdx];
        }

        // Obtain the BSONObj for the given object id.
        const BSONObj& getObject(ElementRep::ObjIdx objIdx) const {
            dassert(objIdx < _objects.size());
            return _objects[objIdx];
        }

        // Insert the given BSONObj and return an ID for it.
        ElementRep::ObjIdx insertObject(const BSONObj& newObj) {
            const size_t objIdx = _objects.size();
            verify(objIdx <= kMaxObjIdx);
            _objects.push_back(newObj);
            if (debug && paranoid) {
                // Force reallocation to catch use after invalidation.
                std::vector<BSONObj> new_objects(_objects);
                _objects.swap(new_objects);
            }
            return objIdx;
        }

        // Given a RepIdx, return the BSONElement that it represents.
        BSONElement getSerializedElement(const ElementRep& rep) const {
            const BSONObj& object = getObject(rep.objIdx);
            return BSONElement(object.objdata() + rep.offset);
        }

        // A helper method that either inserts the field name into the field name heap and
        // updates element.
        void insertFieldName(ElementRep& rep, const StringData& fieldName) {
            dassert(!rep.serialized);
            rep.offset = insertFieldName(fieldName);
        }

        // Retrieve the fieldName, given a rep.
        StringData getFieldName(const ElementRep& rep) const {
            // The root element has no field name.
            if (&rep == &_elements[kRootRepIdx])
                return StringData();

            if (rep.serialized || (rep.objIdx != kInvalidObjIdx))
                return getSerializedElement(rep).fieldName();

            return getFieldName(rep.offset);
        }

        // Retrieve the type, given a rep.
        BSONType getType(const ElementRep& rep) const {
            // The root element is always an Object.
            if (&rep == &_elements[kRootRepIdx])
                return mongo::Object;

            if (rep.serialized || (rep.objIdx != kInvalidObjIdx))
                return getSerializedElement(rep).type();

            return rep.array ? mongo::Array : mongo::Object;
        }

        // Returns true if rep is not an object or array.
        bool isLeaf(const ElementRep& rep) const {
            const BSONType type = getType(rep);
            return ((type != mongo::Object) && (type != mongo::Array));
        }

        // Returns true if rep's value can be provided as a BSONElement.
        bool hasValue(const ElementRep& rep) const {
            // The root element may be marked serialized, but it doesn't have a BSONElement
            // representation.
            if (&rep == &_elements[kRootRepIdx])
                return false;

            return rep.serialized;
        }

        // Return the index of the left child of the Element with index 'index', resolving the
        // left child to a realized Element if it is currently opaque. This may also cause the
        // parent elements child.right entry to be updated.
        Element::RepIdx resolveLeftChild(Element::RepIdx index) {
            dassert(index != kInvalidRepIdx);
            dassert(index != kOpaqueRepIdx);

            // If the left child is anything other than opaque, then we are done here.
            ElementRep* rep = &getElementRep(index);
            if (rep->child.left != kOpaqueRepIdx)
                return rep->child.left;

            // It should be impossible to have an opaque left child and be non-serialized,
            dassert(rep->serialized);
            BSONElement childElt = (
                hasValue(*rep) ?
                getSerializedElement(*rep).embeddedObject() :
                getObject(rep->objIdx)).firstElement();

            if (!childElt.eoo()) {
                ElementRep newRep = makeRep();
                newRep.serialized = true;
                newRep.objIdx = rep->objIdx;
                newRep.offset =
                    getElementOffset(getObject(rep->objIdx), childElt);
                newRep.parent = index;
                newRep.sibling.right = kOpaqueRepIdx;
                // If this new object has possible substructure, mark its children as opaque.
                if (!isLeaf(newRep)) {
                    newRep.child.left = kOpaqueRepIdx;
                    newRep.child.right = kOpaqueRepIdx;
                }
                // Calling insertElement invalidates rep since insertElement may cause a
                // reallocation of the element vector. After calling insertElement, we
                // reacquire rep.
                const Element::RepIdx inserted = insertElement(newRep);
                rep = &getElementRep(index);
                rep->child.left = inserted;
            } else {
                rep->child.left = kInvalidRepIdx;
                rep->child.right = kInvalidRepIdx;
            }

            dassert(rep->child.left != kOpaqueRepIdx);
            return rep->child.left;
        }

        // Return the index of the right child of the Element with index 'index', resolving any
        // opaque nodes. Note that this may require resolving all of the right siblings of the
        // left child.
        Element::RepIdx resolveRightChild(Element::RepIdx index) {
            dassert(index != kInvalidRepIdx);
            dassert(index != kOpaqueRepIdx);

            Element::RepIdx current = getElementRep(index).child.right;
            if (current == kOpaqueRepIdx) {
                current = resolveLeftChild(index);
                while (current != kInvalidRepIdx) {
                    Element::RepIdx next = resolveRightSibling(current);
                    if (next == kInvalidRepIdx)
                        break;
                    current = next;
                }

                // The resolveRightSibling calls should have eventually updated this nodes right
                // child pointer to point to the node we are about to return.
                dassert(getElementRep(index).child.right == current);
            }

            return current;
        }

        // Return the index of the right sibling of the Element with index 'index', resolving
        // the right sibling to a realized Element if it is currently opaque.
        Element::RepIdx resolveRightSibling(Element::RepIdx index) {
            dassert(index != kInvalidRepIdx);
            dassert(index != kOpaqueRepIdx);

            // If the right sibling is anything other than opaque, then we are done here.
            ElementRep* rep = &getElementRep(index);
            if (rep->sibling.right != kOpaqueRepIdx)
                return rep->sibling.right;

            BSONElement elt = getSerializedElement(*rep);
            BSONElement rightElt(elt.rawdata() + elt.size());

            if (!rightElt.eoo()) {
                ElementRep newRep = makeRep();
                newRep.serialized = true;
                newRep.objIdx = rep->objIdx;
                newRep.offset =
                    getElementOffset(getObject(rep->objIdx), rightElt);
                newRep.parent = rep->parent;
                newRep.sibling.left = index;
                newRep.sibling.right = kOpaqueRepIdx;
                // If this new object has possible substructure, mark its children as opaque.
                if (!isLeaf(newRep)) {
                    newRep.child.left = kOpaqueRepIdx;
                    newRep.child.right = kOpaqueRepIdx;
                }
                // Calling insertElement invalidates rep since insertElement may cause a
                // reallocation of the element vector. After calling insertElement, we
                // reacquire rep.
                const Element::RepIdx inserted = insertElement(newRep);
                rep = &getElementRep(index);
                rep->sibling.right = inserted;
            } else {
                rep->sibling.right = kInvalidRepIdx;
                // If we have found the end of this object, then our (necessarily existing)
                // parent's necessarily opaque right child is now determined to be us.
                dassert(rep->parent <= kMaxRepIdx);
                ElementRep& parentRep = getElementRep(rep->parent);
                dassert(parentRep.child.right == kOpaqueRepIdx);
                parentRep.child.right = index;
            }

            dassert(rep->sibling.right != kOpaqueRepIdx);
            return rep->sibling.right;
        }

        // Find the ElementRep at index 'index', and mark it and all of its currently
        // serialized parents as non-serialized.
        void deserialize(Element::RepIdx index) {
            while (index != kInvalidRepIdx) {
                ElementRep& rep = getElementRep(index);
                // It does not make sense for leaf Elements to become deserialized, and
                // requests to do so indicate a bug in the implementation of the library.
                dassert(!isLeaf(rep));
                if (!rep.serialized)
                    break;
                rep.serialized = false;
                index = rep.parent;
            }
        }

        inline bool doesNotAlias(const StringData& s) const {
            // StringData may come from either the field name heap or the leaf builder.
            return !inLeafBuilder(s.rawData()) && !inFieldNameHeap(s.rawData());
        }

        inline bool doesNotAlias(const BSONElement& e) const {
            // A BSONElement could alias the leaf builder.
            return !inLeafBuilder(e.rawdata());
        }

        inline bool doesNotAlias(const BSONObj& o) const {
            // A BSONObj could alias the leaf buildr.
            return !inLeafBuilder(o.objdata());
        }

        // Returns true if 'data' points within the leaf BufBuilder.
        inline bool inLeafBuilder(const char* data) const {
            if (_leafBuf.len() == 0)
                return false;
            // TODO: Write up something documenting that the following is technically UB due
            // to illegality of comparing pointers to different aggregates for ordering. Also,
            // do we need to do anything to prevent the optimizer from compiling this out on
            // that basis? I've seen clang do that. We may need to declare these volatile. On
            // the other hand, these should only be being called under a dassert, so the
            // optimizer is maybe not in play, and the UB is unlikely to be a problem in
            // practice.
            const char* const start = _leafBuf.buf();
            const char* const end = start + _leafBuf.len();
            return (data >= start) && (data < end);
        }

        // Returns true if 'data' points within the field name heap.
        inline bool inFieldNameHeap(const char* data) const {
            if (_fieldNames.empty())
                return false;
            const char* const start = &_fieldNames.front();
            const char* const end = &_fieldNames.back();
            return (data >= start) && (data < end);
        }

        void reserveDamageEvents(size_t expectedEvents) {
            if (_damages)
                _damages->reserve(expectedEvents);
        }

        bool getInPlaceUpdates(DamageVector* damages, const char** source, size_t* size) {

            // If some operations were not in-place, set source to NULL and return false to
            // inform upstream that we are not returning in-place result data.
            if (!_damages) {
                damages->clear();
                *source = NULL;
                return false;
            }

            // Set up the source and source size out parameters.
            *source = _objects[0].objdata();
            if (size)
                *size = _objects[0].objsize();

            // Swap our damage event queue with upstream, and reset ours to an empty vector. In
            // princple, we can do another round of in-place updates.
            damages->swap(*_damages);
            _damages->clear();
            return true;
        }

        void disableInPlaceUpdates() {
            _damages.reset();
        }

        Document::InPlaceMode getCurrentInPlaceMode() const {
            return (_damages ? Document::kInPlaceEnabled : kInPlaceDisabled);
        }

        bool isInPlaceModeEnabled() const {
            return getCurrentInPlaceMode() == Document::kInPlaceEnabled;
        }

        void recordDamageEvent(DamageEvent::OffsetSizeType targetOffset,
                               DamageEvent::OffsetSizeType sourceOffset,
                               size_t size) {
            _damages->push_back(DamageEvent());
            _damages->back().targetOffset = targetOffset;
            _damages->back().sourceOffset = sourceOffset;
            _damages->back().size = size;
            if (debug && paranoid) {
                // Force damage events to new addresses to catch invalidation errors.
                DamageVector new_damages(*_damages);
                _damages->swap(new_damages);
            }
        }

    private:

        // Insert the given field name into the field name heap, and return an ID for this
        // field name.
        int32_t insertFieldName(const StringData& fieldName) {
            const uint32_t id = _fieldNames.size();
            if (!fieldName.empty())
                _fieldNames.insert(
                    _fieldNames.end(),
                    fieldName.rawData(),
                    fieldName.rawData() + fieldName.size());
            _fieldNames.push_back('\0');
            if (debug && paranoid) {
                // Force names to new addresses to catch invalidation errors.
                std::vector<char> new_fieldNames(_fieldNames);
                _fieldNames.swap(new_fieldNames);
            }
            return id;
        }

        // Retrieve the field name with the given id.
        StringData getFieldName(uint32_t fieldNameId) const {
            dassert(fieldNameId < _fieldNames.size());
            return &_fieldNames[fieldNameId];
        }

        std::vector<ElementRep> _elements;
        std::vector<BSONObj> _objects;
        std::vector<char> _fieldNames;

        // We own a BufBuilder to avoid BSONObjBuilder's ref-count mechanism which would throw
        // off our offset calculations.
        BufBuilder _leafBuf;
        BSONObjBuilder _leafBuilder;

        // Queue of damage events if in-place updates are possible.
        boost::scoped_ptr<DamageVector> _damages;
    };

    Status Element::addSiblingLeft(Element e) {
        verify(ok());
        verify(e.ok());
        verify(_doc == e._doc);

        Document::Impl& impl = getDocument().getImpl();
        ElementRep& newRep = impl.getElementRep(e._repIdx);

        // check that new element roots a clean subtree.
        if (!canAttach(e._repIdx, newRep))
            return getAttachmentError(newRep);

        ElementRep& thisRep = impl.getElementRep(_repIdx);

        dassert(thisRep.parent != kOpaqueRepIdx);
        if (thisRep.parent == kInvalidRepIdx)
            return Status(
                ErrorCodes::IllegalOperation,
                "Attempt to add a sibling to an element without a parent");

        ElementRep& parentRep = impl.getElementRep(thisRep.parent);
        dassert(!impl.isLeaf(parentRep));

        impl.disableInPlaceUpdates();

        // The new element shares our parent.
        newRep.parent = thisRep.parent;

        // We are the new element's right sibling.
        newRep.sibling.right = _repIdx;

        // The new element's left sibling is our left sibling.
        newRep.sibling.left = thisRep.sibling.left;

        // If the new element has a left sibling after the adjustments above, then that left
        // sibling must be updated to have the new element as its right sibling.
        if (newRep.sibling.left != kInvalidRepIdx)
            impl.getElementRep(thisRep.sibling.left).sibling.right = e._repIdx;

        // The new element becomes our left sibling.
        thisRep.sibling.left = e._repIdx;

        // If we were our parent's left child, then we no longer are. Make the new right
        // sibling the right child.
        if (parentRep.child.left == _repIdx)
            parentRep.child.left = e._repIdx;

        impl.deserialize(thisRep.parent);

        return Status::OK();
    }

    Status Element::addSiblingRight(Element e) {
        verify(ok());
        verify(e.ok());
        verify(_doc == e._doc);

        Document::Impl& impl = getDocument().getImpl();
        ElementRep* newRep = &impl.getElementRep(e._repIdx);

        // check that new element roots a clean subtree.
        if (!canAttach(e._repIdx, *newRep))
            return getAttachmentError(*newRep);

        ElementRep* thisRep = &impl.getElementRep(_repIdx);

        dassert(thisRep->parent != kOpaqueRepIdx);
        if (thisRep->parent == kInvalidRepIdx)
            return Status(
                ErrorCodes::IllegalOperation,
                "Attempt to add a sibling to an element without a parent");

        ElementRep* parentRep = &impl.getElementRep(thisRep->parent);
        dassert(!impl.isLeaf(*parentRep));

        impl.disableInPlaceUpdates();

        // If our current right sibling is opaque it needs to be resolved. This will invalidate
        // our reps so we need to reacquire them.
        Element::RepIdx rightSiblingIdx = thisRep->sibling.right;
        if (rightSiblingIdx == kOpaqueRepIdx) {
            rightSiblingIdx = impl.resolveRightSibling(_repIdx);
            dassert(rightSiblingIdx != kOpaqueRepIdx);
            newRep = &impl.getElementRep(e._repIdx);
            thisRep = &impl.getElementRep(_repIdx);
            parentRep = &impl.getElementRep(thisRep->parent);
        }

        // The new element shares our parent.
        newRep->parent = thisRep->parent;

        // We are the new element's left sibling.
        newRep->sibling.left = _repIdx;

        // The new element right sibling is our right sibling.
        newRep->sibling.right = rightSiblingIdx;

        // The new element becomes our right sibling.
        thisRep->sibling.right = e._repIdx;

        // If the new element has a right sibling after the adjustments above, then that right
        // sibling must be updated to have the new element as its left sibling.
        if (newRep->sibling.right != kInvalidRepIdx)
            impl.getElementRep(rightSiblingIdx).sibling.left = e._repIdx;

        // If we were our parent's right child, then we no longer are. Make the new right
        // sibling the right child.
        if (parentRep->child.right == _repIdx)
            parentRep->child.right = e._repIdx;

        impl.deserialize(thisRep->parent);

        return Status::OK();
    }

    Status Element::remove() {
        verify(ok());
        Document::Impl& impl = getDocument().getImpl();

        // We need to realize any opaque right sibling, because we are going to need to set its
        // left sibling. Do this before acquiring thisRep since otherwise we would potentially
        // invalidate it.
        impl.resolveRightSibling(_repIdx);

        ElementRep& thisRep = impl.getElementRep(_repIdx);

        if (thisRep.parent == kInvalidRepIdx)
            return Status(ErrorCodes::IllegalOperation, "trying to remove a parentless element");
        impl.disableInPlaceUpdates();

        // If our right sibling is not the end of the object, then set its left sibling to be
        // our left sibling.
        if (thisRep.sibling.right != kInvalidRepIdx)
            impl.getElementRep(thisRep.sibling.right).sibling.left = thisRep.sibling.left;

        // Similarly, if our left sibling is not the beginning of the obejct, then set its
        // right sibling to be our right sibling.
        if (thisRep.sibling.left != kInvalidRepIdx) {
            ElementRep& leftRep = impl.getElementRep(thisRep.sibling.left);
            leftRep.sibling.right = thisRep.sibling.right;
        }

        // If this element was our parent's right child, then our left sibling is the new right
        // child.
        ElementRep& parentRep = impl.getElementRep(thisRep.parent);
        if (parentRep.child.right == _repIdx)
            parentRep.child.right = thisRep.sibling.left;

        // Similarly, if this element was our parent's left child, then our right sibling is
        // the new left child.
        if (parentRep.child.left == _repIdx)
            parentRep.child.left = thisRep.sibling.right;

        impl.deserialize(thisRep.parent);

        // The Element becomes detached.
        thisRep.parent = kInvalidRepIdx;
        thisRep.sibling.left = kInvalidRepIdx;
        thisRep.sibling.right = kInvalidRepIdx;

        return Status::OK();
    }

    Status Element::rename(const StringData& newName) {
        verify(ok());
        Document::Impl& impl = getDocument().getImpl();

        if (_repIdx == kRootRepIdx)
            return Status(ErrorCodes::IllegalOperation,
                          "Invalid attempt to rename the root element of a document");

        dassert(impl.doesNotAlias(newName));

        // TODO: Some rename operations may be possible to do in-place.
        impl.disableInPlaceUpdates();

        // Operations below may invalidate thisRep, so we may need to reacquire it.
        ElementRep* thisRep = &impl.getElementRep(_repIdx);

        // For non-leaf serialized elements, we can realize any opaque relatives and then
        // convert ourselves to deserialized.
        if (thisRep->objIdx != kInvalidObjIdx && !impl.isLeaf(*thisRep)) {

            const bool array = (impl.getType(*thisRep) == mongo::Array);

            // Realize any opaque right sibling or left child now, since otherwise we will lose
            // the ability to do so.
            impl.resolveLeftChild(_repIdx);
            impl.resolveRightSibling(_repIdx);

            // The resolve calls above may have invalidated thisRep, we need to reacquire it.
            thisRep = &impl.getElementRep(_repIdx);

            // Set this up as a non-supported deserialized element. We will set the fieldName
            // in the else clause in the block below.
            impl.deserialize(_repIdx);

            thisRep->array = array;

            // TODO: If we ever want to be able to add to the left or right of an opaque object
            // without expanding, this may need to change.
            thisRep->objIdx = kInvalidObjIdx;
        }

        if (impl.hasValue(*thisRep)) {
            // For leaf elements we just create a new Element with the current value and
            // replace. Note that the 'setValue' call below will invalidate thisRep.
            Element replacement = _doc->makeElementWithNewFieldName(newName, *this);
            setValue(&replacement);
        } else {
            // The easy case: just update what our field name offset refers to.
            impl.insertFieldName(*thisRep, newName);
        }

        return Status::OK();
    }

    Element Element::leftChild() const {
        verify(ok());

        // Capturing Document::Impl by non-const ref exploits the constness loophole
        // created by our Impl so that we can let leftChild be lazily evaluated, even for a
        // const Element.
        Document::Impl& impl = _doc->getImpl();
        const Element::RepIdx leftChildIdx = impl.resolveLeftChild(_repIdx);
        dassert(leftChildIdx != kOpaqueRepIdx);
        return Element(_doc, leftChildIdx);
    }

    Element Element::rightChild() const {
        verify(ok());

        // Capturing Document::Impl by non-const ref exploits the constness loophole
        // created by our Impl so that we can let leftChild be lazily evaluated, even for a
        // const Element.
        Document::Impl& impl = _doc->getImpl();
        const Element::RepIdx rightChildIdx = impl.resolveRightChild(_repIdx);
        dassert(rightChildIdx != kOpaqueRepIdx);
        return Element(_doc, rightChildIdx);
    }

    bool Element::hasChildren() const {
        verify(ok());
        // Capturing Document::Impl by non-const ref exploits the constness loophole
        // created by our Impl so that we can let leftChild be lazily evaluated, even for a
        // const Element.
        Document::Impl& impl = _doc->getImpl();
        return impl.resolveLeftChild(_repIdx) != kInvalidRepIdx;
    }

    Element Element::leftSibling() const {
        verify(ok());

        const Document::Impl& impl = getDocument().getImpl();
        const Element::RepIdx leftSibling = impl.getElementRep(_repIdx).sibling.left;
        // If we have a left sibling, we are assured that it has already been expanded.
        dassert(leftSibling != kOpaqueRepIdx);
        return Element(_doc, leftSibling);
    }

    Element Element::rightSibling() const {
        verify(ok());

        // Capturing Document::Impl by non-const ref exploits the constness loophole
        // created by our Impl so that we can let rightSibling be lazily evaluated, even for a
        // const Element.
        Document::Impl& impl = _doc->getImpl();
        const Element::RepIdx rightSiblingIdx = impl.resolveRightSibling(_repIdx);
        dassert(rightSiblingIdx != kOpaqueRepIdx);
        return Element(_doc, rightSiblingIdx);
    }

    Element Element::parent() const {
        verify(ok());
        const Document::Impl& impl = getDocument().getImpl();
        const Element::RepIdx parentIdx = impl.getElementRep(_repIdx).parent;
        dassert(parentIdx != kOpaqueRepIdx);
        return Element(_doc, parentIdx);
    }

    bool Element::hasValue() const {
        verify(ok());
        const Document::Impl& impl = getDocument().getImpl();
        const ElementRep& thisRep = impl.getElementRep(_repIdx);
        return impl.hasValue(thisRep);
    }

    bool Element::isNumeric() const {
        verify(ok());
        const Document::Impl& impl = getDocument().getImpl();
        const ElementRep& thisRep = impl.getElementRep(_repIdx);
        const BSONType type = impl.getType(thisRep);
        return ((type == mongo::NumberLong) ||
                (type == mongo::NumberInt) ||
                (type == mongo::NumberDouble));
    }

    bool Element::isIntegral() const {
        verify(ok());
        const Document::Impl& impl = getDocument().getImpl();
        const ElementRep& thisRep = impl.getElementRep(_repIdx);
        const BSONType type = impl.getType(thisRep);
        return ((type == mongo::NumberLong) ||
                (type == mongo::NumberInt));
    }

    const BSONElement Element::getValue() const {
        const Document::Impl& impl = getDocument().getImpl();
        const ElementRep& thisRep = impl.getElementRep(_repIdx);
        if (impl.hasValue(thisRep))
            return impl.getSerializedElement(thisRep);
        return BSONElement();
    }

    int Element::compareWithElement(const ConstElement& other, bool considerFieldName) const {
        verify(ok());
        verify(other.ok());

        // Short circuit a tautological compare.
        if ((_repIdx == other.getIdx()) && (_doc == &other.getDocument()))
            return 0;

        // If either Element can represent its current value as a BSONElement, then we can
        // obtain its value and use compareWithBSONElement. If both Elements have a
        // representation as a BSONElement, compareWithBSONElement will notice that the first
        // argument has a value and delegate to BSONElement::woCompare.

        const Document::Impl& impl = getDocument().getImpl();
        const ElementRep& thisRep = impl.getElementRep(_repIdx);

        // Subtle: we must negate the comparison result here because we are reversing the
        // argument order in this call.
        //
        // TODO: Andy has suggested that this may not be legal since woCompare is not reflexive
        // in all cases.
        if (impl.hasValue(thisRep))
            return -other.compareWithBSONElement(
                impl.getSerializedElement(thisRep), considerFieldName);

        const Document::Impl& oimpl = other.getDocument().getImpl();
        const ElementRep& otherRep = oimpl.getElementRep(other.getIdx());

        if (oimpl.hasValue(otherRep))
            return compareWithBSONElement(
                oimpl.getSerializedElement(otherRep), considerFieldName);

        // Leaf elements should always have a value, so we should only be dealing with Objects
        // or Arrays here.
        dassert(!impl.isLeaf(thisRep));
        dassert(!oimpl.isLeaf(otherRep));

        // Obtain the canonical types for this Element and the BSONElement, if they are
        // different use the difference as the result. Please see BSONElement::woCompare for
        // details. We know that thisRep is not a number, so we don't need to check that
        // particular case.
        const int leftCanonType = canonicalizeBSONType(impl.getType(thisRep));
        const int rightCanonType = canonicalizeBSONType(oimpl.getType(otherRep));
        const int diffCanon = leftCanonType - rightCanonType;
        if (diffCanon != 0)
            return diffCanon;

        // If we are considering field names, and the field names do not compare as equal,
        // return the field name ordering as the element ordering.
        if (considerFieldName) {
            const int fnamesComp = impl.getFieldName(thisRep).compare(oimpl.getFieldName(otherRep));
            if (fnamesComp != 0)
                return fnamesComp;
        }

        const bool considerChildFieldNames =
            (impl.getType(thisRep) != mongo::Array) &&
            (oimpl.getType(otherRep) != mongo::Array);

        // We are dealing with either two objects, or two arrays. We need to consider the child
        // elements individually. We walk two iterators forward over the children and compare
        // them. Length mismatches are handled by checking early for reaching the end of the
        // children.
        ConstElement thisIter = leftChild();
        ConstElement otherIter = other.leftChild();

        while (true) {
            if (!thisIter.ok())
                return !otherIter.ok() ? 0 : -1;
            if (!otherIter.ok())
                return 1;

            const int result = thisIter.compareWithElement(otherIter, considerChildFieldNames);
            if (result != 0)
                return result;

            thisIter = thisIter.rightSibling();
            otherIter = otherIter.rightSibling();
        }
    }

    int Element::compareWithBSONElement(const BSONElement& other, bool considerFieldName) const {
        verify(ok());

        const Document::Impl& impl = getDocument().getImpl();
        const ElementRep& thisRep = impl.getElementRep(_repIdx);

        // If we have a representation as a BSONElement, we can just use BSONElement::woCompare
        // to do the entire comparison.
        if (impl.hasValue(thisRep))
            return impl.getSerializedElement(thisRep).woCompare(other, considerFieldName);

        // Leaf elements should always have a value, so we should only be dealing with Objects
        // or Arrays here.
        dassert(!impl.isLeaf(thisRep));

        // Obtain the canonical types for this Element and the BSONElement, if they are
        // different use the difference as the result. Please see BSONElement::woCompare for
        // details. We know that thisRep is not a number, so we don't need to check that
        // particular case.
        const int leftCanonType = canonicalizeBSONType(impl.getType(thisRep));
        const int rightCanonType = canonicalizeBSONType(other.type());
        const int diffCanon = leftCanonType - rightCanonType;
        if (diffCanon != 0)
            return diffCanon;

        // If we are considering field names, and the field names do not compare as equal,
        // return the field name ordering as the element ordering.
        if (considerFieldName) {
            const int fnamesComp = impl.getFieldName(thisRep).compare(other.fieldName());
            if (fnamesComp != 0)
                return fnamesComp;
        }

        const bool considerChildFieldNames =
            (impl.getType(thisRep) != mongo::Array) &&
            (other.type() != mongo::Array);

        return compareWithBSONObj(other.Obj(), considerChildFieldNames);
    }

    int Element::compareWithBSONObj(const BSONObj& other, bool considerFieldName) const {
        verify(ok());

        const Document::Impl& impl = getDocument().getImpl();
        const ElementRep& thisRep = impl.getElementRep(_repIdx);
        verify(!impl.isLeaf(thisRep));

        // We are dealing with either two objects, or two arrays. We need to consider the child
        // elements individually. We walk two iterators forward over the children and compare
        // them. Length mismatches are handled by checking early for reaching the end of the
        // children.
        ConstElement thisIter = leftChild();
        BSONObjIterator otherIter(other);

        while (true) {
            const BSONElement otherVal = otherIter.next();

            if (!thisIter.ok())
                return otherVal.eoo() ? 0 : -1;
            if (otherVal.eoo())
                return 1;

            const int result = thisIter.compareWithBSONElement(otherVal, considerFieldName);
            if (result != 0)
                return result;

            thisIter = thisIter.rightSibling();
        }
    }

    void Element::writeTo(BSONObjBuilder* const builder) const {
        verify(ok());
        const Document::Impl& impl = getDocument().getImpl();
        const ElementRep& thisRep = impl.getElementRep(_repIdx);
        verify(impl.getType(thisRep) == mongo::Object);
        if (thisRep.parent == kInvalidRepIdx && _repIdx == kRootRepIdx) {
            // If this is the root element, then we need to handle it differently, since it
            // doesn't have a field name and should embed directly, rather than as an object.
            writeChildren(builder);
        } else {
            writeElement(builder);
        }
    }

    void Element::writeArrayTo(BSONArrayBuilder* const builder) const {
        verify(ok());
        const Document::Impl& impl = getDocument().getImpl();
        const ElementRep& thisRep = impl.getElementRep(_repIdx);
        verify(impl.getType(thisRep) == mongo::Array);
        return writeChildren(builder);
    }

    Status Element::setValueDouble(const double value) {
        // TODO: Template-ize this so we don't need to write this logic everwhere. The only
        // variation is the 'makeElementX' call and the argument types.

        verify(ok());
        Document::Impl& impl = getDocument().getImpl();
        const ElementRep* thisRep = &impl.getElementRep(_repIdx);

        // Create a dummy element. If we can do in-place updates, we will use this to sew into
        // the element graph. Otherwise, we will use a different element, created below.
        bool inPlace = false;
        Element newValue = getDocument().end();

        if (impl.isInPlaceModeEnabled()) {

            // In place updates are currently enabled. We can do an in-place update to an
            // element that is serialized and is not in the leaf heap.
            const bool inLeafHeap = (thisRep->objIdx == kLeafObjIdx);
            const bool hasValue = impl.hasValue(*thisRep);

            // TODO: In the future, we can replace values in the leaf heap if they are of the
            // same size as the origin was. For now, we don't support that.
            if (hasValue && !inLeafHeap) {

                // Create a new double element in the leaf heap. There is no need to copy the
                // fieldname, since the fieldname and leafheap are in different storage areas.
                const StringData fieldName = impl.getFieldName(*thisRep);
                newValue = getDocument().makeElementDouble(fieldName, value);

                // Reacquire thisRep, since it may have been reallocated by makeElement.
                thisRep = &impl.getElementRep(_repIdx);

                // See if the new Element can be recorded as an in-place update.

                const ElementRep& newRep = impl.getElementRep(newValue._repIdx);
                dassert(impl.hasValue(newRep));

                // Get the BSONElement representations of the existing and new value, so we can
                // check if they are size compatible.
                BSONElement thisElt = impl.getSerializedElement(*thisRep);
                BSONElement newElt = impl.getSerializedElement(newRep);

                if (thisElt.size() == newElt.size()) {

                    inPlace = true;

                    // The old and new elements are size compatible. Compute the base offsets
                    // of each BSONElement in the object in which it resides. We use these to
                    // calculate the source and target offsets in the damage entries we are
                    // going to write.

                    const DamageEvent::OffsetSizeType targetBaseOffset =
                        getElementOffset(impl.getObject(thisRep->objIdx), thisElt);

                    const DamageEvent::OffsetSizeType sourceBaseOffset =
                        getElementOffset(impl.getObject(newRep.objIdx), newElt);

                    // If this is a type change, record a damage event for the new type.
                    if (thisElt.type() != newElt.type()) {
                        impl.recordDamageEvent(targetBaseOffset, sourceBaseOffset, 1);
                    }

                    dassert(thisElt.fieldNameSize() == newElt.fieldNameSize());
                    dassert(thisElt.valuesize() == newElt.valuesize());

                    // Record a damage event for the new value data.
                    impl.recordDamageEvent(
                        targetBaseOffset + thisElt.fieldNameSize() + 1,
                        sourceBaseOffset + thisElt.fieldNameSize() + 1,
                        thisElt.valuesize());
                }
            }
        }

        if (!newValue.ok()) {
            // If we didn't build a new element yet, do so now.
            const std::string fieldNameCopy = impl.getFieldName(*thisRep).toString();
            // TODO: Can we hoist this above so we only call makeElement once in this function?
            newValue = getDocument().makeElementDouble(fieldNameCopy, value);
        }

        // Attach the new element into the graph.
        return setValue(&newValue, inPlace);
    }

    Status Element::setValueString(const StringData& value) {
        verify(ok());
        Document::Impl& impl = getDocument().getImpl();
        dassert(impl.doesNotAlias(value));

        const ElementRep& thisRep = impl.getElementRep(_repIdx);
        const std::string fieldNameCopy = impl.getFieldName(thisRep).toString();
        Element newValue = getDocument().makeElementString(fieldNameCopy, value);
        return setValue(&newValue);
    }

    Status Element::setValueObject(const BSONObj& value) {
        verify(ok());
        Document::Impl& impl = getDocument().getImpl();
        dassert(impl.doesNotAlias(value));

        const ElementRep& thisRep = impl.getElementRep(_repIdx);
        const std::string fieldNameCopy = impl.getFieldName(thisRep).toString();
        Element newValue = getDocument().makeElementObject(fieldNameCopy, value);
        return setValue(&newValue);
    }

    Status Element::setValueArray(const BSONObj& value) {
        verify(ok());
        Document::Impl& impl = getDocument().getImpl();
        dassert(impl.doesNotAlias(value));

        const ElementRep& thisRep = impl.getElementRep(_repIdx);
        const std::string fieldNameCopy = impl.getFieldName(thisRep).toString();
        Element newValue = getDocument().makeElementArray(fieldNameCopy, value);
        return setValue(&newValue);
    }

    Status Element::setValueBinary(const uint32_t len, mongo::BinDataType binType,
                                   const void* const data) {
        verify(ok());
        Document::Impl& impl = getDocument().getImpl();
        // TODO: Alias check for binary data?
        const ElementRep& thisRep = impl.getElementRep(_repIdx);
        const std::string fieldNameCopy = impl.getFieldName(thisRep).toString();
        Element newValue = getDocument().makeElementBinary(
            fieldNameCopy, len, binType, data);
        return setValue(&newValue);
    }

    Status Element::setValueUndefined() {
        verify(ok());
        Document::Impl& impl = getDocument().getImpl();
        const ElementRep& thisRep = impl.getElementRep(_repIdx);
        const std::string fieldNameCopy = impl.getFieldName(thisRep).toString();
        Element newValue = getDocument().makeElementUndefined(fieldNameCopy);
        return setValue(&newValue);
    }

    Status Element::setValueOID(const OID value) {
        verify(ok());
        Document::Impl& impl = getDocument().getImpl();
        const ElementRep& thisRep = impl.getElementRep(_repIdx);
        const std::string fieldNameCopy = impl.getFieldName(thisRep).toString();
        Element newValue = getDocument().makeElementOID(fieldNameCopy, value);
        return setValue(&newValue);
    }

    Status Element::setValueBool(const bool value) {
        verify(ok());
        Document::Impl& impl = getDocument().getImpl();
        const ElementRep* thisRep = &impl.getElementRep(_repIdx);

        // NOTE: See setValueDouble for detailed comments.

        bool inPlace = false;
        Element newValue = getDocument().end();

        if (impl.isInPlaceModeEnabled()) {

            const bool inLeafHeap = (thisRep->objIdx == kLeafObjIdx);
            const bool hasValue = impl.hasValue(*thisRep);

            if (hasValue && !inLeafHeap) {

                const StringData fieldName = impl.getFieldName(*thisRep);
                newValue = getDocument().makeElementBool(fieldName, value);

                thisRep = &impl.getElementRep(_repIdx);

                const ElementRep& newRep = impl.getElementRep(newValue._repIdx);
                dassert(impl.hasValue(newRep));

                BSONElement thisElt = impl.getSerializedElement(*thisRep);
                BSONElement newElt = impl.getSerializedElement(newRep);

                if (thisElt.size() == newElt.size()) {

                    inPlace = true;

                    const DamageEvent::OffsetSizeType targetBaseOffset =
                        getElementOffset(impl.getObject(thisRep->objIdx), thisElt);

                    const DamageEvent::OffsetSizeType sourceBaseOffset =
                        getElementOffset(impl.getObject(newRep.objIdx), newElt);

                    if (thisElt.type() != newElt.type()) {
                        impl.recordDamageEvent(targetBaseOffset, sourceBaseOffset, 1);
                    }

                    dassert(thisElt.fieldNameSize() == newElt.fieldNameSize());
                    dassert(thisElt.valuesize() == newElt.valuesize());

                    impl.recordDamageEvent(
                        targetBaseOffset + thisElt.fieldNameSize() + 1,
                        sourceBaseOffset + thisElt.fieldNameSize() + 1,
                        thisElt.valuesize());
                }
            }
        }

        if (!newValue.ok()) {
            const std::string fieldNameCopy = impl.getFieldName(*thisRep).toString();
            newValue = getDocument().makeElementBool(fieldNameCopy, value);
        }

        return setValue(&newValue, inPlace);
    }

    Status Element::setValueDate(const Date_t value) {
        verify(ok());
        Document::Impl& impl = getDocument().getImpl();
        const ElementRep& thisRep = impl.getElementRep(_repIdx);
        const std::string fieldNameCopy = impl.getFieldName(thisRep).toString();
        Element newValue = getDocument().makeElementDate(fieldNameCopy, value);
        return setValue(&newValue);
    }

    Status Element::setValueNull() {
        verify(ok());
        Document::Impl& impl = getDocument().getImpl();
        const ElementRep& thisRep = impl.getElementRep(_repIdx);
        const std::string fieldNameCopy = impl.getFieldName(thisRep).toString();
        Element newValue = getDocument().makeElementNull(fieldNameCopy);
        return setValue(&newValue);
    }

    Status Element::setValueRegex(const StringData& re, const StringData& flags) {
        verify(ok());
        Document::Impl& impl = getDocument().getImpl();
        dassert(impl.doesNotAlias(re));
        dassert(impl.doesNotAlias(flags));
        const ElementRep& thisRep = impl.getElementRep(_repIdx);
        const std::string fieldNameCopy = impl.getFieldName(thisRep).toString();
        Element newValue = getDocument().makeElementRegex(fieldNameCopy, re, flags);
        return setValue(&newValue);
    }

    Status Element::setValueDBRef(const StringData& ns, const OID oid) {
        verify(ok());
        Document::Impl& impl = getDocument().getImpl();
        dassert(impl.doesNotAlias(ns));
        const ElementRep& thisRep = impl.getElementRep(_repIdx);
        const std::string fieldNameCopy = impl.getFieldName(thisRep).toString();
        Element newValue = getDocument().makeElementDBRef(fieldNameCopy, ns, oid);
        return setValue(&newValue);
    }

    Status Element::setValueCode(const StringData& value) {
        verify(ok());
        Document::Impl& impl = getDocument().getImpl();
        dassert(impl.doesNotAlias(value));
        const ElementRep& thisRep = impl.getElementRep(_repIdx);
        const std::string fieldNameCopy = impl.getFieldName(thisRep).toString();
        Element newValue = getDocument().makeElementCode(fieldNameCopy, value);
        return setValue(&newValue);
    }

    Status Element::setValueSymbol(const StringData& value) {
        verify(ok());
        Document::Impl& impl = getDocument().getImpl();
        dassert(impl.doesNotAlias(value));
        const ElementRep& thisRep = impl.getElementRep(_repIdx);
        const std::string fieldNameCopy = impl.getFieldName(thisRep).toString();
        Element newValue = getDocument().makeElementSymbol(fieldNameCopy, value);
        return setValue(&newValue);
    }

    Status Element::setValueCodeWithScope(const StringData& code, const BSONObj& scope) {
        verify(ok());
        Document::Impl& impl = getDocument().getImpl();
        dassert(impl.doesNotAlias(code));
        dassert(impl.doesNotAlias(scope));
        const ElementRep& thisRep = impl.getElementRep(_repIdx);
        const std::string fieldNameCopy = impl.getFieldName(thisRep).toString();
        Element newValue = getDocument().makeElementCodeWithScope(
            fieldNameCopy, code, scope);
        return setValue(&newValue);
    }

    Status Element::setValueInt(const int32_t value) {
        verify(ok());
        Document::Impl& impl = getDocument().getImpl();
        const ElementRep* thisRep = &impl.getElementRep(_repIdx);

        // NOTE: See setValueDouble for detailed comments.

        bool inPlace = false;
        Element newValue = getDocument().end();

        if (impl.isInPlaceModeEnabled()) {

            const bool inLeafHeap = (thisRep->objIdx == kLeafObjIdx);
            const bool hasValue = impl.hasValue(*thisRep);

            if (hasValue && !inLeafHeap) {

                const StringData fieldName = impl.getFieldName(*thisRep);
                newValue = getDocument().makeElementInt(fieldName, value);

                thisRep = &impl.getElementRep(_repIdx);

                const ElementRep& newRep = impl.getElementRep(newValue._repIdx);
                dassert(impl.hasValue(newRep));

                BSONElement thisElt = impl.getSerializedElement(*thisRep);
                BSONElement newElt = impl.getSerializedElement(newRep);

                if (thisElt.size() == newElt.size()) {

                    inPlace = true;

                    const DamageEvent::OffsetSizeType targetBaseOffset =
                        getElementOffset(impl.getObject(thisRep->objIdx), thisElt);

                    const DamageEvent::OffsetSizeType sourceBaseOffset =
                        getElementOffset(impl.getObject(newRep.objIdx), newElt);

                    if (thisElt.type() != newElt.type()) {
                        impl.recordDamageEvent(targetBaseOffset, sourceBaseOffset, 1);
                    }

                    dassert(thisElt.fieldNameSize() == newElt.fieldNameSize());
                    dassert(thisElt.valuesize() == newElt.valuesize());

                    impl.recordDamageEvent(
                        targetBaseOffset + thisElt.fieldNameSize() + 1,
                        sourceBaseOffset + thisElt.fieldNameSize() + 1,
                        thisElt.valuesize());
                }
            }
        }

        if (!newValue.ok()) {
            const std::string fieldNameCopy = impl.getFieldName(*thisRep).toString();
            newValue = getDocument().makeElementInt(fieldNameCopy, value);
        }

        return setValue(&newValue, inPlace);
    }

    Status Element::setValueTimestamp(const OpTime value) {
        verify(ok());
        Document::Impl& impl = getDocument().getImpl();
        const ElementRep& thisRep = impl.getElementRep(_repIdx);
        const std::string fieldNameCopy = impl.getFieldName(thisRep).toString();
        Element newValue = getDocument().makeElementTimestamp(fieldNameCopy, value);
        return setValue(&newValue);
    }

    Status Element::setValueLong(const int64_t value) {
        verify(ok());
        Document::Impl& impl = getDocument().getImpl();
        const ElementRep* thisRep = &impl.getElementRep(_repIdx);

        // NOTE: See setValueDouble for detailed comments.

        bool inPlace = false;
        Element newValue = getDocument().end();

        if (impl.isInPlaceModeEnabled()) {

            const bool inLeafHeap = (thisRep->objIdx == kLeafObjIdx);
            const bool hasValue = impl.hasValue(*thisRep);

            if (hasValue && !inLeafHeap) {

                const StringData fieldName = impl.getFieldName(*thisRep);
                newValue = getDocument().makeElementLong(fieldName, value);

                thisRep = &impl.getElementRep(_repIdx);

                const ElementRep& newRep = impl.getElementRep(newValue._repIdx);
                dassert(impl.hasValue(newRep));

                BSONElement thisElt = impl.getSerializedElement(*thisRep);
                BSONElement newElt = impl.getSerializedElement(newRep);

                if (thisElt.size() == newElt.size()) {

                    inPlace = true;

                    const DamageEvent::OffsetSizeType targetBaseOffset =
                        getElementOffset(impl.getObject(thisRep->objIdx), thisElt);

                    const DamageEvent::OffsetSizeType sourceBaseOffset =
                        getElementOffset(impl.getObject(newRep.objIdx), newElt);

                    if (thisElt.type() != newElt.type()) {
                        impl.recordDamageEvent(targetBaseOffset, sourceBaseOffset, 1);
                    }

                    dassert(thisElt.fieldNameSize() == newElt.fieldNameSize());
                    dassert(thisElt.valuesize() == newElt.valuesize());

                    impl.recordDamageEvent(
                        targetBaseOffset + thisElt.fieldNameSize() + 1,
                        sourceBaseOffset + thisElt.fieldNameSize() + 1,
                        thisElt.valuesize());
                }
            }
        }

        if (!newValue.ok()) {
            const std::string fieldNameCopy = impl.getFieldName(*thisRep).toString();
            newValue = getDocument().makeElementLong(fieldNameCopy, value);
        }

        return setValue(&newValue, inPlace);
    }

    Status Element::setValueMinKey() {
        verify(ok());
        Document::Impl& impl = getDocument().getImpl();
        const ElementRep& thisRep = impl.getElementRep(_repIdx);
        const std::string fieldNameCopy = impl.getFieldName(thisRep).toString();
        Element newValue = getDocument().makeElementMinKey(fieldNameCopy);
        return setValue(&newValue);
    }

    Status Element::setValueMaxKey() {
        verify(ok());
        Document::Impl& impl = getDocument().getImpl();
        const ElementRep& thisRep = impl.getElementRep(_repIdx);
        const std::string fieldNameCopy = impl.getFieldName(thisRep).toString();
        Element newValue = getDocument().makeElementMaxKey(fieldNameCopy);
        return setValue(&newValue);
    }

    Status Element::setValueBSONElement(const BSONElement& value) {
        verify(ok());
        dassert(getDocument().getImpl().doesNotAlias(value));

        switch(value.type()) {
        case EOO:
            return Status(ErrorCodes::IllegalOperation, "Can't set Element value to EOO");
        case NumberDouble:
            return setValueDouble(value._numberDouble());
        case String:
            return setValueString(StringData(value.valuestr(), value.valuestrsize() - 1));
        case Object:
            return setValueObject(value.Obj());
        case Array:
            return setValueArray(value.Obj());
        case BinData: {
            int len = 0;
            const char* binData = value.binData(len);
            return setValueBinary(len, value.binDataType(), binData);
        }
        case Undefined:
            return setValueUndefined();
        case jstOID:
            return setValueOID(value.__oid());
        case Bool:
            return setValueBool(value.boolean());
        case Date:
            return setValueDate(value.date());
        case jstNULL:
            return setValueNull();
        case RegEx:
            return setValueRegex(value.regex(), value.regexFlags());
        case DBRef:
            return setValueDBRef(value.dbrefNS(), value.dbrefOID());
        case Code:
            return setValueCode(StringData(value.valuestr(), value.valuestrsize() - 1));
        case Symbol:
            return setValueSymbol(StringData(value.valuestr(), value.valuestrsize() - 1));
        case CodeWScope:
            return setValueCodeWithScope(value.codeWScopeCode(), value.codeWScopeObject());
        case NumberInt:
            return setValueInt(value._numberInt());
        case Timestamp:
            return setValueTimestamp(value._opTime());
        case NumberLong:
            return setValueLong(value._numberLong());
        case MinKey:
            return setValueMinKey();
        case MaxKey:
            return setValueMaxKey();
        default:
            verify(false);
        }
    }

    Status Element::setValueSafeNum(const SafeNum value) {
        verify(ok());
        switch (value.type()) {
        case mongo::NumberInt:
            return setValueInt(value._value.int32Val);
        case mongo::NumberLong:
            return setValueLong(value._value.int64Val);
        case mongo::NumberDouble:
            return setValueDouble(value._value.doubleVal);
        default:
            verify(false);
        }
    }

    bool Element::ok() const {
        return ((_doc != NULL) && (_repIdx <= kMaxRepIdx));
    }

    BSONType Element::getType() const {
        verify(ok());
        const Document::Impl& impl = getDocument().getImpl();
        const ElementRep& thisRep = impl.getElementRep(_repIdx);
        return impl.getType(thisRep);
    }

    StringData Element::getFieldName() const {
        verify(ok());
        const Document::Impl& impl = getDocument().getImpl();
        const ElementRep& thisRep = impl.getElementRep(_repIdx);
        return impl.getFieldName(thisRep);
    }

    Status Element::addChild(Element e, bool front) {
        // No need to verify(ok()) since we are only called from methods that have done so.
        dassert(ok());

        verify(e.ok());
        verify(_doc == e._doc);

        Document::Impl& impl = getDocument().getImpl();
        ElementRep& newRep = impl.getElementRep(e._repIdx);

        // check that new element roots a clean subtree.
        if (!canAttach(e._repIdx, newRep))
            return getAttachmentError(newRep);

        // Check that this element is eligible for children.
        ElementRep& thisRep = impl.getElementRep(_repIdx);
        if (impl.isLeaf(thisRep))
            return Status(
                ErrorCodes::IllegalOperation,
                "Attempt to add a child element to a non-object element");

        impl.disableInPlaceUpdates();

        // TODO: In both of the following cases, we call two public API methods each. We can
        // probably do better by writing this explicitly here and drying it with the public
        // addSiblingLeft and addSiblingRight implementations.
        if (front) {
            // TODO: It is cheap to get the left child. However, it still means creating a rep
            // for it. Can we do better?
            Element lc = leftChild();
            if (lc.ok())
                return lc.addSiblingLeft(e);
        } else {
            // TODO: It is expensive to get the right child, since we have to build reps for
            // all of the opaque children. But in principle, we don't really need them. Could
            // we potentially add this element as a right child, leaving its left sibling
            // opaque? We would at minimum need to update leftSibling, which currently assumes
            // that your left sibling is never opaque. But adding new Elements to the end is a
            // quite common operation, so it would be nice if we could do this efficiently.
            Element rc = rightChild();
            if (rc.ok())
                return rc.addSiblingRight(e);
        }

        // It must be the case that we have no children, so the new element becomes both the
        // right and left child of this node.
        dassert((thisRep.child.left == kInvalidRepIdx) && (thisRep.child.right == kInvalidRepIdx));
        thisRep.child.left = thisRep.child.right = e._repIdx;
        newRep.parent = _repIdx;
        impl.deserialize(_repIdx);
        return Status::OK();
    }

    Status Element::setValue(Element* value, bool inPlace) {
        // No need to verify(ok()) since we are only called from methods that have done so.
        dassert(ok());

        if (_repIdx == kRootRepIdx)
            return Status(ErrorCodes::IllegalOperation, "Cannot call setValue on the root object");

        Document::Impl& impl = getDocument().getImpl();

        if (!inPlace)
            impl.disableInPlaceUpdates();

        // Establish our right sibling in case it is opaque. Otherwise, we would lose the
        // ability to do so after the modifications below. It is important that this occur
        // before we acquire thisRep and valueRep since otherwise we would potentially
        // invalidate them.
        impl.resolveRightSibling(_repIdx);

        ElementRep& thisRep = impl.getElementRep(_repIdx);
        ElementRep& valueRep = impl.getElementRep(value->_repIdx);

        // If we are not rootish, then wire in the new value among our relations.
        if (thisRep.parent != kInvalidRepIdx) {
            valueRep.parent = thisRep.parent;
            valueRep.sibling.left = thisRep.sibling.left;
            valueRep.sibling.right = thisRep.sibling.right;
        }

        // Copy the rep for value to our slot so that our repIdx is unmodified and fixup the
        // passed in Element to alias us since we now own the value.
        thisRep = valueRep;
        value->_repIdx = _repIdx;

        // Be nice and clear out the source rep to make debugging easier.
        valueRep = makeRep();

        impl.deserialize(thisRep.parent);
        return Status::OK();
    }


    namespace {

        // A helper for Element::writeElement below. For cases where we are building inside an
        // array, we want to ignore field names. So the specialization for BSONArrayBuilder ignores
        // the third parameter.
        template<typename Builder>
        struct SubBuilder;

        template<>
        struct SubBuilder<BSONObjBuilder> {
            SubBuilder(BSONObjBuilder* builder, BSONType type, const StringData& fieldName)
                : buffer(
                    (type == mongo::Array) ?
                    builder->subarrayStart(fieldName) :
                    builder->subobjStart(fieldName)) {}
            BufBuilder& buffer;
        };

        template<>
        struct SubBuilder<BSONArrayBuilder> {
            SubBuilder(BSONArrayBuilder* builder, BSONType type, const StringData&)
                : buffer(
                    (type == mongo::Array) ?
                    builder->subarrayStart() :
                    builder->subobjStart()) {}
            BufBuilder& buffer;
        };

    } // namespace

    template<typename Builder>
    void Element::writeElement(Builder* builder, const StringData* fieldName) const {
        // No need to verify(ok()) since we are only called from methods that have done so.
        dassert(ok());

        const Document::Impl& impl = getDocument().getImpl();
        const ElementRep& thisRep = impl.getElementRep(_repIdx);

        if (impl.hasValue(thisRep)) {
            BSONElement element = impl.getSerializedElement(thisRep);
            if (fieldName)
                builder->appendAs(element, *fieldName);
            else
                builder->append(element);
        } else {
            const BSONType type = impl.getType(thisRep);
            const StringData subName = fieldName ? *fieldName : impl.getFieldName(thisRep);
            SubBuilder<Builder> subBuilder(builder, type, subName);
            if (type == mongo::Array) {
                BSONArrayBuilder child_builder(subBuilder.buffer);
                writeChildren(&child_builder);
                child_builder.doneFast();
            } else if (type == mongo::Object) {
                BSONObjBuilder child_builder(subBuilder.buffer);
                writeChildren(&child_builder);
                child_builder.doneFast();
            } else {
                // This would only occur on a dirtied leaf, which should never occur.
                verify(false);
            }
        }
    }

    template<typename Builder>
    void Element::writeChildren(Builder* builder) const {
        // No need to verify(ok()) since we are only called from methods that have done so.
        dassert(ok());

        // TODO: In theory, I think we can walk rightwards building a write region from all
        // serialized embedded children that share an obj id and form a contiguous memory
        // region. For arrays we would need to know something about how many elements we wrote
        // that way so that the indexes would come out right.
        //
        // Also in theory instead of walking all the way right, we can walk right until we hit
        // an opaque node. Then we can bulk copy the opaque region, maybe? Probably that
        // doesn't work for arrays.
        //
        // However, both of the above ideas involve walking the memory twice: once two build
        // the copy region, and another time to actually copy it. It is unclear if this is
        // better than just walking it once with the recursive solution.
        Element current = leftChild();
        while (current.ok()) {
            current.writeElement(builder);
            current = current.rightSibling();
        }
    }

    Document::Document()
        : _impl(new Impl(Document::kInPlaceDisabled))
        , _root(makeElementObject(StringData(kRootFieldName, StringData::LiteralTag()))) {
        dassert(_root._repIdx == kRootRepIdx);
    }

    Document::Document(const BSONObj& value, InPlaceMode inPlaceMode)
        : _impl(new Impl(inPlaceMode))
        , _root(makeElementObject(StringData(kRootFieldName, StringData::LiteralTag()), value)) {
        dassert(_root._repIdx == kRootRepIdx);
    }

    Document::~Document() {}

    void Document::reserveDamageEvents(size_t expectedEvents) {
        return getImpl().reserveDamageEvents(expectedEvents);
    }

    bool Document::getInPlaceUpdates(DamageVector* damages,
                                     const char** source, size_t* size) {
        return getImpl().getInPlaceUpdates(damages, source, size);
    }

    void Document::disableInPlaceUpdates() {
        return getImpl().disableInPlaceUpdates();
    }

    Document::InPlaceMode Document::getCurrentInPlaceMode() const {
        return getImpl().getCurrentInPlaceMode();
    }

    Element Document::makeElementDouble(const StringData& fieldName, const double value) {
        Impl& impl = getImpl();
        dassert(impl.doesNotAlias(fieldName));

        BSONObjBuilder& builder = impl.leafBuilder();
        const int leafRef = builder.len();
        builder.append(fieldName, value);
        return Element(this, impl.insertLeafElement(leafRef));
    }

    Element Document::makeElementString(const StringData& fieldName, const StringData& value) {
        Impl& impl = getImpl();
        dassert(impl.doesNotAlias(fieldName));
        dassert(impl.doesNotAlias(value));

        BSONObjBuilder& builder = impl.leafBuilder();
        const int leafRef = builder.len();
        builder.append(fieldName, value);
        return Element(this, impl.insertLeafElement(leafRef));
    }

    Element Document::makeElementObject(const StringData& fieldName) {
        Impl& impl = getImpl();
        dassert(impl.doesNotAlias(fieldName));

        ElementRep newElt = makeRep();
        impl.insertFieldName(newElt, fieldName);
        return Element(this, impl.insertElement(newElt));
    }

    Element Document::makeElementObject(const StringData& fieldName, const BSONObj& value) {
        Impl& impl = getImpl();
        dassert(impl.doesNotAlias(fieldName));
        dassert(impl.doesNotAlias(value));

        Element::RepIdx newEltIdx = kInvalidRepIdx;
        // A cheap hack to detect that this Object Element is for the root.
        if (fieldName.rawData() == &kRootFieldName[0]) {
            ElementRep newElt = makeRep();
            // A BSONObj provided for the root Element is stored in _objects rather than being
            // copied like all other BSONObjs.
            newElt.objIdx = impl.insertObject(value);
            impl.insertFieldName(newElt, fieldName);
            // Strictly, the following is a lie: the root isn't serialized, because it doesn't
            // have a contiguous fieldname. However, it is a useful fiction to pretend that it
            // is, so we can easily check if we have a 'pristine' document state by checking if
            // the root is marked as serialized.
            newElt.serialized = true;
            newEltIdx = impl.insertElement(newElt);
        } else {
            // Copy the provided values into the leaf builder.
            BSONObjBuilder& builder = impl.leafBuilder();
            const int leafRef = builder.len();
            builder.append(fieldName, value);
            newEltIdx = impl.insertLeafElement(leafRef);
        }
        ElementRep& newElt = impl.getElementRep(newEltIdx);
        newElt.child.left = kOpaqueRepIdx;
        newElt.child.right = kOpaqueRepIdx;
        return Element(this, newEltIdx);
    }

    Element Document::makeElementArray(const StringData& fieldName) {
        Impl& impl = getImpl();
        dassert(impl.doesNotAlias(fieldName));

        ElementRep newElt = makeRep();
        newElt.array = true;
        impl.insertFieldName(newElt, fieldName);
        return Element(this, impl.insertElement(newElt));
    }

    Element Document::makeElementArray(const StringData& fieldName, const BSONObj& value) {
        Impl& impl = getImpl();
        dassert(impl.doesNotAlias(fieldName));

        // Copy the provided array values into the leaf builder.
        BSONObjBuilder& builder = impl.leafBuilder();
        const int leafRef = builder.len();
        builder.appendArray(fieldName, value);
        Element::RepIdx newEltIdx = impl.insertLeafElement(leafRef);
        ElementRep& newElt = impl.getElementRep(newEltIdx);
        newElt.child.left = kOpaqueRepIdx;
        newElt.child.right = kOpaqueRepIdx;
        return Element(this, newEltIdx);
    }

    Element Document::makeElementBinary(const StringData& fieldName,
                                        const uint32_t len,
                                        const mongo::BinDataType binType,
                                        const void* const data) {
        Impl& impl = getImpl();
        dassert(impl.doesNotAlias(fieldName));
        // TODO: Alias check 'data'?

        BSONObjBuilder& builder = impl.leafBuilder();
        const int leafRef = builder.len();
        builder.appendBinData(fieldName, len, binType, data);
        return Element(this, impl.insertLeafElement(leafRef));
    }

    Element Document::makeElementUndefined(const StringData& fieldName) {
        Impl& impl = getImpl();
        dassert(impl.doesNotAlias(fieldName));

        BSONObjBuilder& builder = impl.leafBuilder();
        const int leafRef = builder.len();
        builder.appendUndefined(fieldName);
        return Element(this, impl.insertLeafElement(leafRef));
    }

    Element Document::makeElementOID(const StringData& fieldName, const OID value) {
        Impl& impl = getImpl();
        dassert(impl.doesNotAlias(fieldName));

        BSONObjBuilder& builder = impl.leafBuilder();
        const int leafRef = builder.len();
        builder.append(fieldName, value);
        return Element(this, impl.insertLeafElement(leafRef));
    }

    Element Document::makeElementBool(const StringData& fieldName, const bool value) {
        Impl& impl = getImpl();
        dassert(impl.doesNotAlias(fieldName));

        BSONObjBuilder& builder = impl.leafBuilder();
        const int leafRef = builder.len();
        builder.appendBool(fieldName, value);
        return Element(this, impl.insertLeafElement(leafRef));
    }

    Element Document::makeElementDate(const StringData& fieldName, const Date_t value) {
        Impl& impl = getImpl();
        dassert(impl.doesNotAlias(fieldName));

        BSONObjBuilder& builder = impl.leafBuilder();
        const int leafRef = builder.len();
        builder.appendDate(fieldName, value);
        return Element(this, impl.insertLeafElement(leafRef));
    }

    Element Document::makeElementNull(const StringData& fieldName) {
        Impl& impl = getImpl();
        dassert(impl.doesNotAlias(fieldName));

        BSONObjBuilder& builder = impl.leafBuilder();
        const int leafRef = builder.len();
        builder.appendNull(fieldName);
        return Element(this, impl.insertLeafElement(leafRef));
    }

    Element Document::makeElementRegex(const StringData& fieldName,
                                       const StringData& re,
                                       const StringData& flags) {
        Impl& impl = getImpl();
        dassert(impl.doesNotAlias(fieldName));
        dassert(impl.doesNotAlias(re));
        dassert(impl.doesNotAlias(flags));

        BSONObjBuilder& builder = impl.leafBuilder();
        const int leafRef = builder.len();
        builder.appendRegex(fieldName, re, flags);
        return Element(this, impl.insertLeafElement(leafRef));
    }

    Element Document::makeElementDBRef(const StringData& fieldName,
                                       const StringData& ns, const OID value) {
        Impl& impl = getImpl();
        dassert(impl.doesNotAlias(fieldName));
        BSONObjBuilder& builder = impl.leafBuilder();
        const int leafRef = builder.len();
        builder.appendDBRef(fieldName, ns, value);
        return Element(this, impl.insertLeafElement(leafRef));
    }

    Element Document::makeElementCode(const StringData& fieldName, const StringData& value) {
        Impl& impl = getImpl();
        dassert(impl.doesNotAlias(fieldName));
        dassert(impl.doesNotAlias(value));

        BSONObjBuilder& builder = impl.leafBuilder();
        const int leafRef = builder.len();
        builder.appendCode(fieldName, value);
        return Element(this, impl.insertLeafElement(leafRef));
    }

    Element Document::makeElementSymbol(const StringData& fieldName, const StringData& value) {
        Impl& impl = getImpl();
        dassert(impl.doesNotAlias(fieldName));
        dassert(impl.doesNotAlias(value));

        BSONObjBuilder& builder = impl.leafBuilder();
        const int leafRef = builder.len();
        builder.appendSymbol(fieldName, value);
        return Element(this, impl.insertLeafElement(leafRef));
    }

    Element Document::makeElementCodeWithScope(const StringData& fieldName,
                                               const StringData& code, const BSONObj& scope) {
        Impl& impl = getImpl();
        dassert(impl.doesNotAlias(fieldName));
        dassert(impl.doesNotAlias(code));
        dassert(impl.doesNotAlias(scope));

        BSONObjBuilder& builder = impl.leafBuilder();
        const int leafRef = builder.len();
        builder.appendCodeWScope(fieldName, code, scope);
        return Element(this, impl.insertLeafElement(leafRef));
    }

    Element Document::makeElementInt(const StringData& fieldName, const int32_t value) {
        Impl& impl = getImpl();
        dassert(impl.doesNotAlias(fieldName));

        BSONObjBuilder& builder = impl.leafBuilder();
        const int leafRef = builder.len();
        builder.append(fieldName, value);
        return Element(this, impl.insertLeafElement(leafRef));
    }

    Element Document::makeElementTimestamp(const StringData& fieldName, const OpTime value) {
        Impl& impl = getImpl();
        dassert(impl.doesNotAlias(fieldName));

        BSONObjBuilder& builder = impl.leafBuilder();
        const int leafRef = builder.len();
        builder.appendTimestamp(fieldName, value.asDate());
        return Element(this, impl.insertLeafElement(leafRef));
    }

    Element Document::makeElementLong(const StringData& fieldName, const int64_t value) {
        Impl& impl = getImpl();
        dassert(impl.doesNotAlias(fieldName));

        BSONObjBuilder& builder = impl.leafBuilder();
        const int leafRef = builder.len();
        builder.append(fieldName, static_cast<long long int>(value));
        return Element(this, impl.insertLeafElement(leafRef));
    }

    Element Document::makeElementMinKey(const StringData& fieldName) {
        Impl& impl = getImpl();
        dassert(impl.doesNotAlias(fieldName));

        BSONObjBuilder& builder = impl.leafBuilder();
        const int leafRef = builder.len();
        builder.appendMinKey(fieldName);
        return Element(this, impl.insertLeafElement(leafRef));
    }

    Element Document::makeElementMaxKey(const StringData& fieldName) {
        Impl& impl = getImpl();
        dassert(impl.doesNotAlias(fieldName));

        BSONObjBuilder& builder = impl.leafBuilder();
        const int leafRef = builder.len();
        builder.appendMaxKey(fieldName);
        return Element(this, impl.insertLeafElement(leafRef));
    }

    Element Document::makeElement(const BSONElement& value) {
        return makeElementWithNewFieldName(value.fieldName(), value);
    }

    Element Document::makeElementWithNewFieldName(const StringData& fieldName,
                                                  const BSONElement& value) {

        dassert(getImpl().doesNotAlias(fieldName));
        dassert(getImpl().doesNotAlias(value));

        // These are in the same order as the bsonspec.org specification. Please keep them that
        // way.
        switch(value.type()) {
        case EOO:
            verify(false);
        case NumberDouble:
            return makeElementDouble(fieldName, value._numberDouble());
        case String:
            return makeElementString(fieldName,
                                     StringData(value.valuestr(), value.valuestrsize() - 1));
        case Object:
            return makeElementObject(fieldName, value.Obj());
        case Array:
            // As above.
            return makeElementArray(fieldName, value.Obj());
        case BinData: {
            int len = 0;
            const char* binData = value.binData(len);
            return makeElementBinary(fieldName, len, value.binDataType(), binData);
        }
        case Undefined:
            return makeElementUndefined(fieldName);
        case jstOID:
            return makeElementOID(fieldName, value.__oid());
        case Bool:
            return makeElementBool(fieldName, value.boolean());
        case Date:
            return makeElementDate(fieldName, value.date());
        case jstNULL:
            return makeElementNull(fieldName);
        case RegEx:
            return makeElementRegex(fieldName, value.regex(), value.regexFlags());
        case DBRef:
            return makeElementDBRef(fieldName, value.dbrefNS(), value.dbrefOID());
        case Code:
            return makeElementCode(fieldName,
                                   StringData(value.valuestr(), value.valuestrsize() - 1));
        case Symbol:
            return makeElementSymbol(fieldName,
                                     StringData(value.valuestr(), value.valuestrsize() - 1));
        case CodeWScope:
            return makeElementCodeWithScope(fieldName,
                                            value.codeWScopeCode(), value.codeWScopeObject());
        case NumberInt:
            return makeElementInt(fieldName, value._numberInt());
        case Timestamp:
            return makeElementTimestamp(fieldName, value._opTime());
        case NumberLong:
            return makeElementLong(fieldName, value._numberLong());
        case MinKey:
            return makeElementMinKey(fieldName);
        case MaxKey:
            return makeElementMaxKey(fieldName);
        default:
            verify(false);
        }
    }

    Element Document::makeElementSafeNum(const StringData& fieldName, SafeNum value) {

        dassert(getImpl().doesNotAlias(fieldName));

        switch (value.type()) {
        case mongo::NumberInt:
            return makeElementInt(fieldName, value._value.int32Val);
        case mongo::NumberLong:
            return makeElementLong(fieldName, value._value.int64Val);
        case mongo::NumberDouble:
            return makeElementDouble(fieldName, value._value.doubleVal);
        default:
            verify(false);
        }
    }

    Element Document::makeElement(ConstElement element) {
        return makeElement(element, NULL);
    }

    Element Document::makeElementWithNewFieldName(const StringData& fieldName,
                                                  ConstElement element) {
        return makeElement(element, &fieldName);
    }

    Element Document::makeElement(ConstElement element, const StringData* fieldName) {
        if (this == &element.getDocument()) {
            // If the Element that we want to build from belongs to this Document, then we have
            // to first copy it to the side, and then back in, since otherwise we might be
            // attempting both read to and write from the underlying BufBuilder simultaneously,
            // which will not work.
            BSONObjBuilder builder;
            element.writeElement(&builder, fieldName);
            BSONObj built = builder.obj();
            BSONElement newElement = built.firstElement();
            return makeElement(newElement);
        } else {
            // If the Element belongs to another document, then we can just stream it into our
            // builder. We still do need to dassert that the field name doesn't alias us
            // somehow.
            Impl& impl = getImpl();
            if (fieldName) {
                dassert(impl.doesNotAlias(*fieldName));
            }
            BSONObjBuilder& builder = impl.leafBuilder();
            const int leafRef = builder.len();
            element.writeElement(&builder, fieldName);
            return Element(this, impl.insertLeafElement(leafRef));
        }
    }

    Element Document::end() {
        return Element(this, kInvalidRepIdx);
    }

    ConstElement Document::end() const {
        return const_cast<Document*>(this)->end();
    }

    inline Document::Impl& Document::getImpl() {
        // Don't use scoped_ptr<Impl>::operator* since it may generate assertions that the
        // pointer is non-null, but we already know that to be always and forever true, and
        // otherwise the assertion code gets spammed into every method that inlines the call to
        // this function. We just dereference the pointer returned from 'get' ourselves.
        return *_impl.get();
    }

    inline const Document::Impl& Document::getImpl() const {
        return *_impl.get();
    }

} // namespace mutablebson
} // namespace mongo
