#include <algorithm>
#include <limits>

#include "json.hpp"
// using json = json;

#include "heaphistory.h"

// Constructors for helper classes.

HeapConflict::HeapConflict(uint32_t tick, uint64_t address, bool alloc)
    : tick_(tick), address_(address), allocation_or_free_(alloc) {}

// The code for the heap history.
HeapHistory::HeapHistory()
    : current_tick_(0),
      global_area_(std::numeric_limits<uint64_t>::max(), 0, 0, 1) {
  setCurrentWindowToGlobal();
}

bool HeapHistory::hasMandatoryJSONElementFields(
    const nlohmann::json &json_element) {
  static std::map<std::string, std::vector<std::string>> mandatory_fields = {
      {"alloc", {"address", "size"}},
      {"free", {"address"}},
      {"filterrange", {"low", "high"}},
      {"event", {}},
      {"rangefree", {"low", "high"}},
      {"address", {"address"}}};
  if (json_element.find("type") == json_element.end()) {
    std::cout << "[E] Failed to find type field" << std::endl;
    return false;
  }
  std::string type = json_element["type"].get<std::string>();
  std::vector<std::string> mandatory = mandatory_fields[type];
  for (const std::string &field : mandatory) {
    if (json_element.find(field) == json_element.end()) {
      std::cout << "[E] Failed to find mandatory field " << field
                << " for type " << type << std::endl;
      return false;
    }
  }
  return true;
}

void HeapHistory::LoadFromJSONStream(std::istream &jsondata) {
  nlohmann::json incoming_data;
  incoming_data << jsondata;

  uint32_t counter = 0;
  for (const auto &json_element : incoming_data) {
    if (!hasMandatoryJSONElementFields(json_element)) {
      continue;
    }
    // The "type" field is mandatory for every event.
    std::string type = json_element["type"].get<std::string>();
    std::string tag = "";
    if (json_element.find("tag") != json_element.end()) {
      tag = json_element["tag"].get<std::string>();
    }
    // A not-too-intrusive gray by default.
    std::string color = "#B0B0B0";
    if (json_element.find("color") != json_element.end()) {
      color = json_element["color"].get<std::string>();
    }

    auto ret = alloc_or_free_tags_.insert(tag);
    const std::string *de_duped_tag = &*(ret.first);

    if (type == "alloc") {
      recordMalloc(json_element["address"].get<uint64_t>(),
                   json_element["size"].get<uint32_t>(), de_duped_tag, 0);
    } else if (type == "free") {
      recordFree(json_element["address"].get<uint64_t>(), de_duped_tag, 0);
    } else if (type == "event") {
      recordEvent(tag, color);
    } else if (type == "rangefree") {
      uint64_t low = json_element["low"].get<uint64_t>();
      uint64_t high = json_element["high"].get<uint64_t>();
      recordFreeRange(low, high, de_duped_tag, 0);
    } else if (type == "address") {
      recordAddress(json_element["address"].get<uint64_t>(), tag, color);
    } else if (type == "filterrange") {
      uint64_t low = json_element["low"].get<uint64_t>();
      uint64_t high = json_element["high"].get<uint64_t>();
      recordFilterRange(low, high);
    }

    fflush(stdout);
  }
  printf("heap_blocks_.size() is %d\n", heap_blocks_.size());
  fflush(stdout);
}

size_t HeapHistory::getActiveBlocks(
    std::vector<std::vector<HeapBlock>::iterator> *active_blocks) {
  // For the moment, implement a naive linear sweep of all heap blocks.
  // This can certainly be made better, but keeping it simple has priority
  // for the moment.
  size_t active_block_count = 0;
  for (std::vector<HeapBlock>::iterator iter = heap_blocks_.begin();
       iter != heap_blocks_.end(); ++iter) {
    if (isBlockActive(*iter)) {
      active_blocks->push_back(iter);
      ++active_block_count;
    }
  }
  return active_block_count;
}

bool HeapHistory::isBlockActive(const HeapBlock &block) {
  return true;
}

void HeapHistory::setCurrentWindow(const HeapWindow &new_window) {
  current_window_.reset(new_window);
}

bool HeapHistory::isEventFiltered(uint64_t address) {
  if (filter_ranges_.empty()) {
    return false;
  }
  for (const auto& filter : filter_ranges_) {
    if ((address >= filter.first) && (address <= filter.second)) {
      return false;
    }
  }
  return true;
}

void HeapHistory::recordMalloc(uint64_t address, size_t size,
                               const std::string *tag, uint8_t heap_id) {
  ++current_tick_;
  if (isEventFiltered(address)) {
    return;
  }

  // Check if there is already a live block at this address.
  if (live_blocks_.find(std::make_pair(address, heap_id)) !=
      live_blocks_.end()) {
    // Record a conflict.
    recordMallocConflict(address, size, heap_id);
    return;
  }
  heap_blocks_.push_back(HeapBlock(current_tick_, size, address, tag));
  this->cached_blocks_sorted_by_address_.clear();

  live_blocks_[std::make_pair(address, heap_id)] = heap_blocks_.size() - 1;

  global_area_.maximum_address_ =
      std::max(address + size, global_area_.maximum_address_);
  global_area_.minimum_address_ =
      std::min(address, global_area_.minimum_address_);
  // Make room for 5% more on the right hand side.
  global_area_.maximum_tick_ =
      (static_cast<double>(current_tick_) * 1.05) + 1.0;
  global_area_.minimum_tick_ = 0;
}

void HeapHistory::recordFree(uint64_t address, const std::string *tag,
                             uint8_t heap_id) {
  // Any event has to clear the sorted HeapBlock cache.
  ++current_tick_;
  if (isEventFiltered(address)) {
    return;
  }
  std::map<std::pair<uint64_t, uint8_t>, size_t>::iterator current_block =
      live_blocks_.find(std::make_pair(address, heap_id));
  if (current_block == live_blocks_.end()) {
    recordFreeConflict(address, heap_id);
    return;
  }
  size_t index = current_block->second;
  heap_blocks_[index].end_tick_ = current_tick_;
  heap_blocks_[index].free_tag_ = tag;
  live_blocks_.erase(current_block);

  // Set the max tick 5% higher than strictly necessary.
  global_area_.maximum_tick_ =
      (static_cast<double>(current_tick_) * 1.05) + 1.0;
}

void HeapHistory::recordFreeRange(uint64_t low_end, uint64_t high_end,
                                  const std::string *tag, uint8_t heap_id) {
  // Find the lower boundary.
  std::map<std::pair<uint64_t, uint8_t>, size_t>::iterator start_block =
      live_blocks_.lower_bound(std::make_pair(low_end, heap_id));
  std::map<std::pair<uint64_t, uint8_t>, size_t>::iterator end_block =
      live_blocks_.upper_bound(std::make_pair(high_end, heap_id));
  std::vector<std::pair<uint64_t, uint8_t>> blocks_to_free;

  while ((start_block != end_block) && (start_block != live_blocks_.end())) {
    uint64_t block_address = start_block->first.first;
    uint8_t block_heap_id = start_block->first.second;
    // We cannot call recordFree inside the loop because modifying the
    // live_block_ map would invalidate our
    // iterators.
    if ((block_heap_id == heap_id) && (block_address >= low_end) &&
        (block_address <= high_end)) {
      blocks_to_free.push_back(std::make_pair(block_address, heap_id));
    }
    ++start_block;
  }
  for (const auto &block : blocks_to_free) {
    recordFree(block.first, tag, block.second);
  }
}

void HeapHistory::recordFilterRange(uint64_t low, uint64_t high) {
  filter_ranges_.push_back(std::make_pair(low, high));
}

void HeapHistory::recordFreeConflict(uint64_t address, uint8_t heap_id) {
  conflicts_.push_back(HeapConflict(current_tick_, address, false));
}

void HeapHistory::recordMallocConflict(uint64_t address, size_t size,
                                       uint8_t heap_id) {
  conflicts_.push_back(HeapConflict(current_tick_, address, true));
}

void HeapHistory::recordRealloc(uint64_t old_address, uint64_t new_address,
                                size_t size, uint8_t heap_id) {
  // How should realloc relations be visualized?
  auto ret = alloc_or_free_tags_.insert("Free'd on reallocation");
  recordFree(old_address, &*(ret.first), heap_id);
  // Should the address perhaps be remembered here?
  ret = alloc_or_free_tags_.insert("Reallocated block");
  recordMalloc(new_address, size, &*(ret.first), heap_id);
}

void HeapHistory::recordEvent(const std::string &event_label,
                              const std::string &color) {
  tick_to_event_strings_[current_tick_] =
      std::make_pair(ColorStringToUint32(color), event_label);
}

uint32_t HeapHistory::ColorStringToUint32(const std::string &color) {
  const char *str = color.c_str() + 1;
  return strtol(str, nullptr, 16);
}

void HeapHistory::recordAddress(uint64_t address, const std::string &label,
                                const std::string &color) {
  address_to_address_strings_[address] =
      std::make_pair(ColorStringToUint32(color), label);
}

void HeapHistory::eventsToVertices(std::vector<HeapVertex> *vertices) {
  // Add two vertices with 0 or 1 on the y axis, and the proper tick on the
  // x axis.
  for (const auto &event : tick_to_event_strings_) {
    uint32_t color = event.second.first;
    float red = static_cast<float>((color & 0xFF0000) >> 16) / 255.0;
    float green = static_cast<float>((color & 0xFF00) >> 8) / 255.0;
    float blue = static_cast<float>(color & 0xFF) / 255.0;

    vertices->push_back(HeapVertex(event.first, 0, QVector3D(red, green, blue)));
    vertices->push_back(HeapVertex(event.first, 1, QVector3D(red, green, blue)));
  }
}

void HeapHistory::addressesToVertices(std::vector<HeapVertex> *vertices) {
  // Add two vertices with 0 or 1 on the y axis, and the proper tick on the
  // x axis.
  for (const auto &event : address_to_address_strings_) {
    uint32_t color = event.second.first;

    float red = static_cast<float>(color & 0xFF0000 >> 16) / 255.0;
    float green = static_cast<float>(color & 0xFF00 >> 8) / 255.0;
    float blue = static_cast<float>(color & 0xFF) / 255.0;

    vertices->push_back(HeapVertex(0, event.first, QVector3D(red, green, blue)));
    vertices->push_back(HeapVertex(1, event.first, QVector3D(red, green, blue)));
  }
}

bool HeapHistory::getEventAtTick(uint32_t tick, std::string *eventstring) {
  const auto iterator = tick_to_event_strings_.find(tick);
  if (iterator == tick_to_event_strings_.end()) {
    // Try an approximate search.
    auto iterator_approx = tick_to_event_strings_.lower_bound(tick-300);
    uint32_t minimum = std::numeric_limits<uint32_t>::max();

    while ((iterator_approx != tick_to_event_strings_.end() &&
           (abs(iterator_approx->first - tick) < 300))) {
      if (abs(iterator_approx->first - tick) <= minimum) {
        minimum = abs(iterator_approx->first - tick);
        *eventstring = iterator_approx->second.second;
      }
      ++iterator_approx;
    }
    if (minimum != std::numeric_limits<uint32_t>::max()) {
      return true;
    } else {
      return false;
    }
  } else {
    *eventstring = iterator->second.second;
    return true;
  }
}

// Write out 6 vertices (for two triangles) into the buffer.
void HeapHistory::HeapBlockToVertices(const HeapBlock &block,
                                      std::vector<HeapVertex> *vertices) {
  block.toVertices(current_tick_, vertices);
}

size_t HeapHistory::heapBlockVerticesForActiveWindow(
    std::vector<HeapVertex> *vertices) {
  size_t active_block_count = 0;
  for (std::vector<HeapBlock>::iterator iter = heap_blocks_.begin();
       iter != heap_blocks_.end(); ++iter) {
    if (isBlockActive(*iter)) {
      HeapBlockToVertices(*iter, vertices);
      ++active_block_count;
    }
  }
  return active_block_count;
}

// Extremely slow O(n) version of testing if a given point lies within any
// block.
bool HeapHistory::getBlockAtSlow(uint64_t address, uint32_t tick,
                                 HeapBlock *result, uint32_t *index) {
  for (std::vector<HeapBlock>::iterator iter = heap_blocks_.begin();
       iter != heap_blocks_.end(); ++iter) {
    if (iter->contains(tick, address)) {
      *result = *iter;
      *index = iter - heap_blocks_.begin();
      return true;
    }
  }
  return false;
}

void HeapHistory::updateCachedSortedIterators() {
  // Makes sure there is a sorted iterator array.
  if (cached_blocks_sorted_by_address_.size() == 0) {
    // Fill the iterator cache.
    for (std::vector<HeapBlock>::iterator iter = heap_blocks_.begin();
         iter != heap_blocks_.end(); ++iter) {
      cached_blocks_sorted_by_address_.push_back(iter);
    }
    auto comparison = [](const std::vector<HeapBlock>::iterator &left,
                         const std::vector<HeapBlock>::iterator &right) {
      // Sort blocks in descending order by address, then tick.
      return (left->address_ > right->address_) ||
             (left->start_tick_ > right->start_tick_);
    };
    std::sort(cached_blocks_sorted_by_address_.begin(),
              cached_blocks_sorted_by_address_.end(), comparison);
  }
}

// Attempts to find a block on the heap at a given address and tick.
// If successful, the provided pointer is filled, and an index into
// the internal heap block vector is provided (this is useful for
// calculating vertex ranges).
//
// XXX: This code is still buggy, and does not seem to find all blocks.
// TODO(thomasdullien): Debug and fix.
//
bool HeapHistory::getBlockAt(uint64_t address, uint32_t tick, HeapBlock *result,
                             uint32_t *index) {
  updateCachedSortedIterators();
  std::pair<uint64_t, uint32_t> val(address, tick);

  // Find the block whose lower left corner is the first (by address, then by
  // tick, descending) to come after the requested point.
  const std::vector<std::vector<HeapBlock>::iterator>::iterator candidate =
      std::lower_bound(cached_blocks_sorted_by_address_.begin(),
                       cached_blocks_sorted_by_address_.end(), val,
                       [this](const std::vector<HeapBlock>::iterator &iterator,
                              const std::pair<uint64_t, uint32_t> &pair) {
                         fflush(stdout);
                         return (pair.first < iterator->address_) ||
                                (pair.second < iterator->start_tick_);
                       });
  if (candidate == cached_blocks_sorted_by_address_.end()) {
    return false;
  }
  // Check that the point lies within the candidate block.
  std::vector<HeapBlock>::iterator block = *candidate;
  if ((address > (block->address_ + block->size_)) ||
      (address < block->address_) || (tick > block->end_tick_) ||
      (tick < block->start_tick_)) {
    return false;
  }
  *result = *block;
  *index = *candidate - heap_blocks_.begin();
  return true;
}

// Provided a displacement (percentage of size of the current window in x and y
// direction), pan the window accordingly.
void HeapHistory::panCurrentWindow(double dx, double dy) {
  current_window_.pan(dx, dy);
}

// Zoom toward a given point on the screen. The point is given in relative
// height / width of the current window, e.g. the center is 0.5, 0.5.
void HeapHistory::zoomToPoint(double dx, double dy, double how_much_x,
                              double how_much_y, long double max_height,
                              long double max_width) {
  current_window_.zoomToPoint(dx, dy, how_much_x, how_much_y, max_height,
                              max_width);
}

/*const ContinuousHeapWindow &
HeapHistory::getGridWindow(uint32_t number_of_lines) {

  // Find the first power-of-two p so that 16 * 2^p > height, and
  // 16 * 2^p2 > width. Then update the grid_window_ variable and
  // return it.
  auto next_greater_pow2 = [number_of_lines](uint64_t limit) {
    uint64_t p = 1;
    while ((number_of_lines / 2) * p < limit) {
      p = p << 1;
    };
    return p;
  };
  uint64_t p1 = next_greater_pow2(current_window_.height());
  uint64_t p2 = next_greater_pow2(current_window_.width());

  auto round_up = [](uint64_t input, uint64_t pow2) {
    return input += (pow2 - (input % pow2));
  };

  grid_rectangle_.maximum_tick_ =
      round_up(current_window_.maximum_tick_, p2 * (number_of_lines / 2));
  grid_rectangle_.maximum_address_ =
      round_up(current_window_.maximum_address_, p1 * number_of_lines / 2);
  grid_rectangle_.minimum_tick_ =
      grid_rectangle_.maximum_tick_ - p2 * number_of_lines;
  grid_rectangle_.minimum_address_ =
      grid_rectangle_.maximum_address_ - p1 * number_of_lines;
  return grid_rectangle_;

}*/
