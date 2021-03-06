#ifndef _SRC_UTILS_H_
#define _SRC_UTILS_H_

#include <stdlib.h> // NULL
#include <stdarg.h> // va_list
#include <stdint.h> // uint32_t
#include <stdio.h> // vsnprintf
#include <string.h> // strncmp, memset
#include <unistd.h> // sysconf or getpagesize

namespace candor {
namespace internal {

#define UNEXPECTED assert(0 && "Unexpected");

inline uint32_t ComputeHash(int64_t key) {
  uint32_t hash = 0;

  // high
  hash += key >> 32;
  hash += (hash << 10);
  hash ^= (hash >> 6);

  // low
  hash += key & 0xffffffff;
  hash += (hash << 10);
  hash ^= (hash >> 6);

  // mixup
  hash += (hash << 3);
  hash ^= (hash >> 11);
  hash += (hash << 15);

  return hash;
}

inline uint32_t ComputeHash(const char* key, uint32_t length) {
  uint32_t hash = 0;
  for (uint32_t i = 0; i < length; i++) {
    hash += key[i];
    hash += (hash << 10);
    hash ^= (hash >> 6);
  }
  hash += (hash << 3);
  hash ^= (hash >> 11);
  hash += (hash << 15);

  return hash;
}


class EmptyClass { };

template <class T, class ItemParent>
class List {
 public:
  class Item : public ItemParent {
   public:
    Item(T value) : value_(value), prev_(NULL), next_(NULL) {
    }

    inline T value() { return value_; }
    inline void value(T value) { value_ = value; }
    inline Item* next() { return next_; }
    inline Item* prev() { return prev_; }
    inline void remove() {
      if (prev_ != NULL) prev_->next_ = next_;
      if (next_ != NULL) next_->prev_ = prev_;
    }

   protected:
    T value_;
    Item* prev_;
    Item* next_;

    friend class List;
  };

  List() : allocated(false), head_(NULL), current_(NULL), length_(0) {
  }


  ~List() {
    if (allocated) {
      while (length_ > 0) delete Shift();
    } else {
      while (length_ > 0) Shift();
    }
  }


  void Push(T item) {
    Item* next = new Item(item);
    next->prev_ = current_;

    if (head_ == NULL) {
      head_ = next;
    } else {
      current_->next_ = next;
    }

    current_ = next;
    length_++;
  }


  void Remove(Item* item) {
    if (current_ == item) current_ = item->prev_;
    if (head_ == item) head_ = item->prev_;
    if (allocated) delete item->value();

    item->remove();
    delete item;

    length_--;
  }


  void Unshift(T item) {
    Item* next = new Item(item);
    next->prev_ = NULL;
    next->next_ = head_;
    head_ = next;
    length_++;
  }


  T Shift() {
    if (head_ == NULL) return NULL;

    Item* tmp = head_;
    T value = head_->value();

    if (head_ == current_) current_ = NULL;
    if (head_->next_ != NULL) head_->next()->prev_ = NULL;

    head_ = head_->next();
    delete tmp;
    length_--;

    return value;
  }


  inline Item* head() { return head_; }
  inline Item* tail() { return current_; }
  inline uint32_t length() { return length_; }

  bool allocated;

 private:
  Item* head_;
  Item* current_;
  uint32_t length_;
};


template <class T, class ItemParent>
class HashMap {
 public:
  typedef void (*EnumerateCallback)(void* map, T value);

  class Item : public ItemParent {
   public:
    Item(const char* key, uint32_t length, T value) : key_(key),
                                                      length_(length),
                                                      value_(value),
                                                      next_(NULL),
                                                      next_linear_(NULL) {
    }

    inline T value() { return value_; }
    inline void value(T value) { value_ = value; }
    inline Item* next() { return next_; }
    inline Item* next_linear() { return next_linear_; }

   protected:
    const char* key_;
    uint32_t length_;
    T value_;

    Item* next_;
    Item* next_linear_;

    friend class HashMap;
  };

  HashMap() : head_(NULL), current_(NULL) {
    memset(&map_, 0, sizeof(map_));
  }


  void Set(const char* key, uint32_t length, T value) {
    uint32_t index = ComputeHash(key, length) & mask_;
    Item* i = map_[index];
    Item* next = new Item(key, length, value);

    // Setup head or append item to linked list
    // (Needed for enumeration)
    if (head_ == NULL) {
      head_ = next;
    } else {
      current_->next_linear_ = next;
    }
    current_ = next;

    if (i == NULL) {
      map_[index] = next;
    } else {
      while (i->next() != NULL) i = i->next();
      i->next_ = next;
    }
  }


  T Get(const char* key, uint32_t length) {
    uint32_t index = ComputeHash(key, length) & mask_;
    Item* i = map_[index];

    while (i != NULL) {
      if (length == i->length_ &&
          strncmp(i->key_, key, length) == 0) {
        return i->value();
      }
      i = i->next();
    }

    return NULL;
  }


  void Enumerate(EnumerateCallback cb) {
    Item* i = head_;

    while (i != NULL) {
      cb(this, i->value());
      i = i->next_linear();
    }
  }

 private:
  static const uint32_t size_ = 64;
  static const uint32_t mask_ = 63;
  Item* map_[size_];
  Item* head_;
  Item* current_;
};


// For debug printing AST and other things
class PrintBuffer {
 public:
  PrintBuffer(char* buffer, int32_t size) : buffer_(buffer),
                                            left_(size),
                                            total_(0) {
  }

  bool Print(const char* format, ...) {
    va_list arguments;
    va_start(arguments, format);
    int32_t written = vsnprintf(buffer_, left_, format, arguments);
    va_end(arguments);
    return Consume(written);
  }

  bool PrintValue(const char* value, int32_t size) {
    Consume(size);
    if (ended()) return false;
    memcpy(buffer_ - size, value, size);

    return !ended();
  }

  bool Consume(int32_t bytes) {
    buffer_ += bytes;
    total_ += bytes;
    left_ -= bytes;

    return !ended();
  }

  void Finalize() {
    if (ended()) return;
    buffer_[total_] = 0;
  }

  inline bool ended() { return left_ <= 0; }
  inline int32_t total() { return total_ <= 0; }

 private:
  char* buffer_;
  int32_t left_;
  int32_t total_;
};



// Find minimum number that's greater than value and is dividable by to
inline uint32_t RoundUp(uint32_t value, uint32_t to) {
  if (value % to == 0) return value;

  return value + to - value % to;
}


inline uint32_t PowerOfTwo(uint32_t value) {
  uint32_t result = 2;

  while (result != 0 && result < value) result <<= 1;

  return result;
}


// Naive only for lexer generated number strings
inline bool StringIsDouble(const char* value, uint32_t length) {
  for (uint32_t index = 0; index < length; index++) {
    if (value[index] == '.') return true;
  }

  return false;
}


inline uint32_t StringGetNumSign(const char* value, uint32_t length, bool* s) {
  uint32_t index = 0;
  // Skip spaces
  while (index < length && value[index] == ' ') index++;

  // Check if we found sign
  if (index < length && value[index] == '-') {
    *s = true;
    index++;
  }

  return index;
}


inline bool is_num(const char c) {
  unsigned char uc = c;

  return uc >= '0' && uc <= '9';
}


inline bool is_hex(const char c) {
  unsigned char uc = c;

  return is_num(c) || (uc >= 'a' && uc <= 'f') || (uc >= 'A' && uc <= 'F');
}


inline int hex_to_num(const char c) {
  switch (c) {
   case '0': return 0;
   case '1': return 1;
   case '2': return 2;
   case '3': return 3;
   case '4': return 4;
   case '5': return 5;
   case '6': return 6;
   case '7': return 7;
   case '8': return 8;
   case '9': return 9;
   case 'a': return 10;
   case 'b': return 11;
   case 'c': return 12;
   case 'd': return 13;
   case 'e': return 14;
   case 'f': return 15;
   case 'A': return 10;
   case 'B': return 11;
   case 'C': return 12;
   case 'D': return 13;
   case 'E': return 14;
   case 'F': return 15;
  }
  return 0;
}


inline int64_t StringToInt(const char* value, uint32_t length) {
  int64_t result = 0;
  bool sign = false;

  uint32_t index = StringGetNumSign(value, length, &sign);
  for (; index < length; index++) {
    if (!is_num(value[index])) break;
    result *= 10;
    result += value[index] - '0';
  }

  return sign ? -result : result;
}


inline double StringToDouble(const char* value, uint32_t length) {
  double integral = 0;
  double floating = 0;
  bool sign = false;

  uint32_t index = StringGetNumSign(value, length, &sign);
  for (; index < length; index++) {
    if (value[index] == '.') break;
    if (!is_num(value[index])) break;
    integral *= 10;
    integral += value[index] - '0';
  }

  if (index < length && value[index] == '.') {
    for (uint32_t i = length - 1; i > index; i--) {
      if (!is_num(value[i])) break;
      floating += value[i] - '0';
      floating /= 10;
    }
  }

  return sign ? -(integral + floating) : integral + floating;
}


inline int GetSourceLineByOffset(const char* source,
                                 uint32_t offset,
                                 int* pos) {
  int result = 1;
  uint32_t line_start = 0;

  for (uint32_t i = 0; i < offset; i++) {
    if (source[i] == '\r') {
      if (i + 1 < offset && source[i] == '\n') i++;
      result++;
      line_start = offset + 1;
    } else if (source[i] == '\n') {
      result++;
      line_start = offset + 1;
    }
  }

  *pos = offset - line_start;

  return result;
}


inline const char* Unescape(const char* value, uint32_t length, uint32_t* res) {
  char* result = new char[length];
  uint32_t offset = 0;
  for (uint32_t i = 0; i < length; i++) {
    if (value[i] == '\\') {
      i++;
      switch (value[i]) {
       case 'b': result[offset] = '\b'; break;
       case 'r': result[offset] = '\r'; break;
       case 'n': result[offset] = '\n'; break;
       case 't': result[offset] = '\t'; break;
       case 'v': result[offset] = '\v'; break;
       case '0': result[offset] = '\0'; break;
       case 'u':
        if (i + 4 < length && is_hex(value[i + 1]) && is_hex(value[i + 2]) &&
            is_hex(value[i + 3]) && is_hex(value[i + 4])) {
          result[offset] = (hex_to_num(value[i + 1]) << 4) +
                           hex_to_num(value[i + 2]);
          offset++;
          result[offset] = (hex_to_num(value[i + 3]) << 4) +
                           hex_to_num(value[i + 4]);
          i += 4;
        } else {
          result[offset] = value[i];
        }
        break;
       case 'x':
        if (i + 2 < length && is_hex(value[i + 1]) && is_hex(value[i + 2])) {
          result[offset] = (hex_to_num(value[i + 1]) << 4) +
                           hex_to_num(value[i + 2]);
          i += 2;
          break;
        }
       default: result[offset] = value[i]; break;
      }
    } else {
      result[offset] = value[i];
    }
    offset++;
  }

  *res = offset;
  return result;
}


inline uint32_t GetPageSize() {
#ifdef __DARWIN
  return getpagesize();
#else
  return sysconf(_SC_PAGE_SIZE);
#endif
}

} // namespace internal
} // namespace candor

#endif // _SRC_UTILS_H_
