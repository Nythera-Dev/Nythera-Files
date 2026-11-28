#pragma once

#include "filemanager/dialogs/modal_dialog_host.h"
#include "filemanager/search_filters.h"

#include <functional>
#include <optional>

namespace filemanager {

  /// Opens the modal calendar picker for search modified-date filters.
  void showSearchDatePicker(
      ModalDialogHost& host, SearchDate initial, bool second, std::function<void(std::optional<SearchDate>)> onSelected
  );

} // namespace filemanager
