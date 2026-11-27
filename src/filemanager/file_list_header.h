#pragma once

#include "core/files/directory_scanner.h"
#include "filemanager/file_columns.h"
#include "filemanager/tab_presentation.h"
#include "wayland/wayland_seat.h"

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

class Button;
class FileManagerTestAccess;
class Flex;
class InputDispatcher;
class Renderer;
class Separator;
class Scrollbar;
class VirtualGridView;
class WaylandConnection;

namespace filemanager {
  inline constexpr float kFileListHeaderHeight = 36.0F;

  class FileManagerSettings;

  class FileListHeaderController {
  public:
    struct Callbacks {
      std::function<void()> onViewStateChanged;
      std::function<void()> onSortChanged;
      std::function<void()> onViewModeOrScaleChanged;
      std::function<void()> onRequestLayout;
      std::function<void()> onRequestRedraw;
      std::function<void(std::string)> onStatus;
      std::function<bool()> canHandlePointer;
      std::function<bool()> canAdjustScale;
      std::function<std::uint32_t()> pointerSerial;
      std::function<float(Renderer&, FileColumn)> measureColumn;
      std::function<void(std::optional<FileColumn>, float, float)> openHeaderMenu;
    };

    FileListHeaderController(
        FileManagerSettings& settings, WaylandConnection& wayland, InputDispatcher& dispatcher, Callbacks callbacks
    );

    void loadInitialState();
    [[nodiscard]] std::unique_ptr<Flex> buildHeader();
    [[nodiscard]] std::unique_ptr<Flex> buildHorizontalScrollbar();
    void attachGrid(VirtualGridView* grid);
    void detachUi();

    void setSortField(FileDialogSortField field);
    void toggleSortOrder();
    void syncSortControls();
    void setGridMode(bool gridMode);
    void adjustViewScale(float value);
    void ensureViewScaleLoaded();
    void reloadColumnPreferences();
    void applyTabViewState(const TabViewState& state, bool search = false);

    void updateColumnHeaders();
    void persistColumnLayout();
    bool applyColumnWidths();
    void arrangeAfterWidthChange(Renderer& renderer);
    void positionColumnMarkers();
    void layoutVisibleMarkers(Renderer& renderer);
    void cancelGesture();
    void requestFit(std::optional<FileColumn> column);
    void applyPendingFit(Renderer& renderer);
    [[nodiscard]] std::optional<FileColumn> columnAt(float x) const;
    [[nodiscard]] float scrollOffset() const { return m_scrollOffset; }
    [[nodiscard]] float contentWidth() const;
    [[nodiscard]] float columnTextInset(FileColumn column) const;
    bool scrollHorizontally(float delta);
    void revealColumn(FileColumn column);
    [[nodiscard]] bool headerAt(float x, float y) const;
    bool handlePointer(const PointerEvent& event, float x, float y);

    [[nodiscard]] bool gridMode() const noexcept { return m_gridMode; }
    [[nodiscard]] float viewScale() const noexcept { return m_viewScale; }
    [[nodiscard]] FileDialogSortField sortField() const noexcept { return m_sortField; }
    [[nodiscard]] FileDialogSortOrder sortOrder() const noexcept { return m_sortOrder; }
    [[nodiscard]] const FileColumns& listColumns() const noexcept { return m_listColumns; }
    [[nodiscard]] const std::vector<FileColumn>& visibleColumns() const noexcept { return m_visibleColumns; }
    [[nodiscard]] bool searchView() const noexcept { return m_searchView; }
    [[nodiscard]] std::string_view columnLabel(FileColumn column) const;
    [[nodiscard]] FileDialogSortField columnSortField(FileColumn column) const;
    [[nodiscard]] FileColumnLayout& columnLayout() noexcept { return m_columnLayout; }
    [[nodiscard]] const FileColumnLayout& columnLayout() const noexcept { return m_columnLayout; }
    [[nodiscard]] bool gestureActive() const noexcept {
      return m_resizingColumn.has_value() || m_columnDrag.has_value();
    }

  private:
    friend class ::FileManagerTestAccess;

    struct ColumnDrag {
      FileColumn column;
      float x;
      float y;
      std::optional<std::size_t> destination;
    };

    void saveWidths();
    [[nodiscard]] float availableWidth() const;
    [[nodiscard]] float stripOrigin() const;
    void setScrollOffset(float offset);
    struct DividerClick {
      FileColumn column;
      std::uint32_t time;
      float x;
    };

    FileManagerSettings& m_settings;
    WaylandConnection& m_wayland;
    InputDispatcher& m_dispatcher;
    Callbacks m_callbacks;

    Flex* m_header = nullptr;
    Flex* m_viewport = nullptr;
    Flex* m_strip = nullptr;
    Flex* m_horizontalTrack = nullptr;
    Scrollbar* m_horizontalScrollbar = nullptr;
    VirtualGridView* m_grid = nullptr;
    std::array<Button*, kFileColumns.size()> m_headers{};
    std::array<Separator*, kFileColumns.size()> m_markers{};
    Separator* m_dropMarker = nullptr;

    std::optional<FileColumn> m_resizingColumn;
    std::optional<FileColumn> m_hoverBoundary;
    std::optional<DividerClick> m_dividerClick;
    std::optional<DividerClick> m_dividerPress;
    std::vector<FileColumn> m_pendingFit;
    float m_scrollOffset = 0.0F;
    std::array<float, 2> m_savedViewOffsets{};
    float m_dragX = 0.0F;
    FileColumns m_columnStart{};
    FileColumns m_listColumns{};
    FileColumns m_preferredColumns{};
    bool m_searchView = false;
    FileColumnLayout m_columnLayout;
    std::vector<FileColumn> m_visibleColumns;
    float m_leftGutter = -1.0F;
    float m_viewportWidth = 0.0F;
    std::optional<ColumnDrag> m_columnDrag;

    FileDialogSortField m_sortField = FileDialogSortField::Name;
    FileDialogSortOrder m_sortOrder = FileDialogSortOrder::Ascending;
    bool m_gridMode = false;
    float m_viewScale = 1.0F;
    bool m_viewScaleLoaded = false;
  };

} // namespace filemanager
