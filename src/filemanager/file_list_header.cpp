#include "filemanager/file_list_header.h"

#include "cursor-shape-v1-client-protocol.h"
#include "filemanager/file_list_columns.h"
#include "filemanager/file_manager_settings.h"
#include "render/core/renderer.h"
#include "render/scene/input_area.h"
#include "render/scene/input_dispatcher.h"
#include "ui/controls/button.h"
#include "ui/controls/flex.h"
#include "ui/controls/label.h"
#include "ui/controls/scrollbar.h"
#include "ui/controls/separator.h"
#include "ui/controls/virtual_grid_view.h"
#include "ui/dialogs/file_entry_row.h"
#include "ui/dialogs/file_view_metrics.h"
#include "ui/palette.h"
#include "ui/style.h"
#include "wayland/wayland_connection.h"

#include <algorithm>
#include <cmath>
#include <linux/input-event-codes.h>

namespace filemanager {

  namespace {
    constexpr float kColumnHeaderHoverOpacity = 0.35F;
    constexpr float kResizeMarkerIdleOpacity = 0.58F;
    constexpr float kDraggedColumnBorderOpacity = 0.7F;
    constexpr float kResizeMarkerHoveredOpacity = 0.82F;

    constexpr std::array kColumnSortFields{FileDialogSortField::Name,     FileDialogSortField::Size,
                                           FileDialogSortField::Modified, FileDialogSortField::Type,
                                           FileDialogSortField::Created,  FileDialogSortField::Accessed};
  } // namespace

  FileListHeaderController::FileListHeaderController(
      FileManagerSettings& settings, WaylandConnection& wayland, InputDispatcher& dispatcher, Callbacks callbacks
  )
      : m_settings(settings), m_wayland(wayland), m_dispatcher(dispatcher), m_callbacks(std::move(callbacks)) {}

  void FileListHeaderController::loadInitialState() {
    const auto viewSettings = m_settings.loadFileView();
    m_gridMode = viewSettings.gridMode;
    m_sortField = viewSettings.sortField;
    m_sortOrder = viewSettings.sortOrder;
    m_settings.loadListColumnWidths(m_preferredColumns, m_searchView);
    m_settings.loadListColumnLayout(m_columnLayout, m_searchView);
    m_listColumns = m_preferredColumns;
  }

  std::unique_ptr<Flex> FileListHeaderController::buildHeader() {
    auto fileHeader = std::make_unique<Flex>();
    m_header = fileHeader.get();
    fileHeader->setVisible(!m_gridMode);
    fileHeader->setParticipatesInLayout(!m_gridMode);
    fileHeader->setExcludeSubtreeFromTabOrder(m_gridMode);
    fileHeader->setMinHeight(kFileListHeaderHeight);
    auto viewport = std::make_unique<Flex>();
    viewport->setParticipatesInLayout(false);
    viewport->setClipChildren(true);
    m_viewport = static_cast<Flex*>(fileHeader->addChild(std::move(viewport)));
    auto strip = std::make_unique<Flex>();
    strip->setParticipatesInLayout(false);
    strip->setMirrorInRtl(false);
    strip->setAlign(FlexAlign::Center);
    strip->setGap(Style::spaceSm);
    strip->setPadding(5.0F, Style::spaceSm);
    m_strip = static_cast<Flex*>(m_viewport->addChild(std::move(strip)));
    const auto styleHeader = [](Button& button) {
      auto headerPalette = Button::defaultPalette(ButtonVariant::Ghost);
      headerPalette.normal = {clearColorSpec(), clearColorSpec(), colorSpecFromRole(ColorRole::OnSurfaceVariant)};
      headerPalette.hover = {
          colorSpecFromRole(ColorRole::Hover, kColumnHeaderHoverOpacity), clearColorSpec(),
          colorSpecFromRole(ColorRole::OnSurface)
      };
      headerPalette.pressed = headerPalette.hover;
      headerPalette.selected =
          Button::ButtonStateColors{clearColorSpec(), clearColorSpec(), colorSpecFromRole(ColorRole::Primary)};
      button.setCustomPalette(std::move(headerPalette));
      button.setFontSize(Style::fontSizeCaption);
      button.setControlHeight(26.0F);
      button.setPadding(0.0F);
      button.setContentAlign(ButtonContentAlign::Start);
      button.setMirrorInRtl(false);
    };
    for (auto column : kFileColumns) {
      const auto index = columnIndex(column);
      auto header = std::make_unique<Button>();
      header->setText(columnLabel(column));
      header->setVariant(ButtonVariant::Ghost);
      header->setOnClick([this, column] { setSortField(columnSortField(column)); });
      header->setClipChildren(true);
      header->label()->setFlexGrow(1.0F);
      header->label()->setMaxLines(1);
      m_headers[index] = header.get();
      styleHeader(*header);
      header->setMinWidth(listColumnWidth(column, 0.0F));
      header->setMaxWidth(listColumnWidth(column, m_listColumns[column]));
      m_strip->addChild(std::move(header));
    }
    const auto makeColumnMarker = [](ColorSpec color) {
      auto node = std::make_unique<Separator>();
      node->setParticipatesInLayout(false);
      node->setHitTestVisible(false);
      node->setZIndex(1);
      node->setOrientation(SeparatorOrientation::VerticalRule);
      node->setThickness(2.0F);
      node->setGradientEdges(true);
      node->setColor(color);
      node->setSize(2.0F, 20.0F);
      return node;
    };
    for (auto& marker : m_markers) {
      auto node = makeColumnMarker(colorSpecFromRole(ColorRole::Primary, kResizeMarkerIdleOpacity));
      marker = node.get();
      m_strip->addChild(std::move(node));
    }
    auto dropMarker = makeColumnMarker(colorSpecFromRole(ColorRole::Primary));
    m_dropMarker = dropMarker.get();
    dropMarker->setZIndex(2);
    dropMarker->setVisible(false);
    m_strip->addChild(std::move(dropMarker));
    return fileHeader;
  }

  std::unique_ptr<Flex> FileListHeaderController::buildHorizontalScrollbar() {
    auto track = std::make_unique<Flex>();
    track->setMinHeight(14.0F);
    track->setVisible(false);
    track->setParticipatesInLayout(false);
    m_horizontalTrack = track.get();
    auto scrollbar = std::make_unique<Scrollbar>();
    scrollbar->setOrientation(ScrollOrientation::Horizontal);
    scrollbar->setParticipatesInLayout(false);
    scrollbar->setOnScrollChanged([this](float offset) { setScrollOffset(offset); });
    m_horizontalScrollbar = static_cast<Scrollbar*>(track->addChild(std::move(scrollbar)));
    return track;
  }

  void FileListHeaderController::attachGrid(VirtualGridView* grid) { m_grid = grid; }

  void FileListHeaderController::detachUi() {
    m_resizingColumn.reset();
    m_hoverBoundary.reset();
    m_columnDrag.reset();
    m_header = nullptr;
    m_grid = nullptr;
    m_headers.fill(nullptr);
    m_markers.fill(nullptr);
    m_dropMarker = nullptr;
    m_leftGutter = -1.0F;
    m_viewportWidth = 0.0F;
    m_viewport = nullptr;
    m_strip = nullptr;
    m_horizontalTrack = nullptr;
    m_horizontalScrollbar = nullptr;
  }

  void FileListHeaderController::ensureViewScaleLoaded() {
    if (!m_viewScaleLoaded) {
      m_viewScaleLoaded = true;
      m_viewScale = m_settings.loadViewScale(m_viewScale);
    }
  }

  void FileListHeaderController::reloadColumnPreferences() {
    m_preferredColumns = FileColumns{};
    m_columnLayout = m_searchView ? kDefaultSearchColumnLayout : FileColumnLayout{};
    m_settings.loadListColumnWidths(m_preferredColumns, m_searchView);
    m_settings.loadListColumnLayout(m_columnLayout, m_searchView);
    m_listColumns = m_preferredColumns;
    m_scrollOffset = 0.0F;
    m_pendingFit.clear();
    m_dividerClick.reset();
    updateColumnHeaders();
  }

  void FileListHeaderController::applyTabViewState(const TabViewState& state, bool search) {
    cancelGesture();
    m_gridMode = state.gridMode;
    m_sortField = state.sortField;
    m_sortOrder = state.sortOrder;
    m_viewScale = state.viewScale;
    m_viewScaleLoaded = true;
    if (m_searchView != search) {
      m_savedViewOffsets[static_cast<std::size_t>(m_searchView)] = m_scrollOffset;
      m_searchView = search;
      reloadColumnPreferences();
      m_scrollOffset = m_savedViewOffsets[static_cast<std::size_t>(search)];
    } else {
      updateColumnHeaders();
    }
    syncSortControls();
  }

  void FileListHeaderController::setSortField(FileDialogSortField field) {
    if (m_sortField == field) {
      toggleSortOrder();
      return;
    }
    m_sortField = field;
    if (m_callbacks.onViewStateChanged)
      m_callbacks.onViewStateChanged();
    if (m_callbacks.onSortChanged)
      m_callbacks.onSortChanged();
    syncSortControls();
  }

  void FileListHeaderController::toggleSortOrder() {
    m_sortOrder = m_sortOrder == FileDialogSortOrder::Ascending ? FileDialogSortOrder::Descending
                                                                : FileDialogSortOrder::Ascending;
    if (m_callbacks.onViewStateChanged)
      m_callbacks.onViewStateChanged();
    if (m_callbacks.onSortChanged)
      m_callbacks.onSortChanged();
    syncSortControls();
  }

  std::string_view FileListHeaderController::columnLabel(FileColumn column) const {
    return (m_searchView ? kSearchFileColumnLabels : kFileColumnLabels)[columnIndex(column)];
  }

  FileDialogSortField FileListHeaderController::columnSortField(FileColumn column) const {
    return m_searchView && column == kSearchLocationSlot ? FileDialogSortField::Location
                                                         : kColumnSortFields[columnIndex(column)];
  }

  void FileListHeaderController::syncSortControls() {
    for (auto column : kFileColumns) {
      const auto index = columnIndex(column);
      if (auto* header = m_headers[index]) {
        const bool selected = m_sortField == columnSortField(column);
        auto text = std::string(columnLabel(column));
        if (selected)
          text += m_sortOrder == FileDialogSortOrder::Ascending ? "  ↑" : "  ↓";
        header->label()->setTextAlign(Style::rtl() ? TextAlign::End : TextAlign::Start);
        header->setText(text);
        header->setTooltip(std::string(columnLabel(column)));
        header->setSelected(selected);
      }
    }
  }

  void FileListHeaderController::setGridMode(bool gridMode) {
    if (m_gridMode == gridMode)
      return;
    cancelGesture();
    m_gridMode = gridMode;
    m_pendingFit.clear();
    m_dividerClick.reset();
    if (m_callbacks.onViewStateChanged)
      m_callbacks.onViewStateChanged();
    updateColumnHeaders();
    if (m_callbacks.onViewModeOrScaleChanged)
      m_callbacks.onViewModeOrScaleChanged();
  }

  void FileListHeaderController::adjustViewScale(float value) {
    if (!m_grid || gestureActive() || (m_callbacks.canAdjustScale && !m_callbacks.canAdjustScale()))
      return;
    const float scale = clampFileViewScale(value);
    if (scale == m_viewScale)
      return;
    m_viewScale = scale;
    if (m_callbacks.onViewModeOrScaleChanged)
      m_callbacks.onViewModeOrScaleChanged();
    if (!m_settings.saveViewScale(m_viewScale, m_searchView) && m_callbacks.onStatus)
      m_callbacks.onStatus("Unable to save file view zoom");
  }

  void FileListHeaderController::updateColumnHeaders() {
    if (!m_header)
      return;
    cancelGesture();
    const bool showHeader = !m_gridMode;
    m_header->setVisible(showHeader);
    m_header->setParticipatesInLayout(showHeader);
    m_header->setExcludeSubtreeFromTabOrder(!showHeader);
    if (!showHeader && m_horizontalTrack) {
      m_horizontalTrack->setVisible(false);
      m_horizontalTrack->setParticipatesInLayout(false);
    }
    m_visibleColumns = m_gridMode ? std::vector<FileColumn>(kDefaultFileColumns.begin(), kDefaultFileColumns.end())
                                  : m_columnLayout.displayed();
    for (auto column : kFileColumns) {
      const auto index = columnIndex(column);
      const bool visible = showHeader && std::ranges::contains(m_visibleColumns, column);
      m_headers[index]->setVisible(visible);
      m_headers[index]->setParticipatesInLayout(visible);
      m_markers[index]->setVisible(visible);
    }
    if (showHeader) {
      for (auto column : m_visibleColumns)
        m_strip->addChild(m_strip->removeChild(m_headers[columnIndex(column)]));
      applyColumnWidths();
      if (m_grid)
        m_grid->notifyDataChanged();
    }
    if (m_callbacks.onRequestLayout)
      m_callbacks.onRequestLayout();
  }

  void FileListHeaderController::persistColumnLayout() {
    if (!m_settings.saveListColumnLayout(m_columnLayout, m_searchView) && m_callbacks.onStatus)
      m_callbacks.onStatus("Unable to save file column layout");
  }

  float FileListHeaderController::columnTextInset(FileColumn column) const {
    if (column == FileColumn::Name)
      return fileViewMetrics(m_viewScale).listIcon + Style::spaceSm;
    return fileColumnTextInset(column == m_visibleColumns.back(), Style::spaceSm);
  }

  float FileListHeaderController::contentWidth() const {
    return listContentWidth(m_listColumns, m_visibleColumns, Style::spaceSm);
  }

  float FileListHeaderController::stripOrigin() const {
    float x = 0, y = 0;
    if (m_header)
      Node::absolutePosition(m_header, x, y);
    return x + std::max(0.0F, m_leftGutter);
  }

  std::optional<FileColumn> FileListHeaderController::columnAt(float x) const {
    const float local = x - stripOrigin();
    if (m_gridMode || local < 0 || local >= availableWidth())
      return std::nullopt;
    for (auto column : m_visibleColumns) {
      const auto bounds = listColumnBounds(column, m_listColumns, m_visibleColumns, Style::spaceSm, m_scrollOffset);
      if (local >= bounds.left && local < bounds.right)
        return column;
    }
    return std::nullopt;
  }

  void FileListHeaderController::setScrollOffset(float offset) {
    offset = std::clamp(offset, 0.0F, std::max(0.0F, contentWidth() - availableWidth()));
    if (offset == m_scrollOffset || gestureActive())
      return;
    m_scrollOffset = offset;
    m_dividerClick.reset();
    if (m_callbacks.onRequestLayout)
      m_callbacks.onRequestLayout();
  }

  bool FileListHeaderController::scrollHorizontally(float delta) {
    if (m_gridMode || contentWidth() <= availableWidth())
      return false;
    setScrollOffset(m_scrollOffset + delta);
    return true;
  }

  bool FileListHeaderController::headerAt(float x, float y) const {
    return m_header && !m_gridMode && m_header->containsScenePoint(x, y);
  }

  void FileListHeaderController::revealColumn(FileColumn column) {
    if (m_gridMode || !std::ranges::contains(m_visibleColumns, column))
      return;
    const auto bounds = listColumnBounds(column, m_listColumns, m_visibleColumns, Style::spaceSm, m_scrollOffset);
    if (bounds.left < 0.0F || bounds.right > availableWidth())
      setScrollOffset(m_scrollOffset + bounds.left - Style::spaceSm);
  }

  bool FileListHeaderController::applyColumnWidths() {
    if (!m_header || m_gridMode || m_visibleColumns.empty())
      return false;
    float leftGutter = 0.0F;
    float viewportWidth = m_header->width();
    if (m_grid && m_grid->focusArea()->width() > 0.0F) {
      float headerX = 0.0F, headerY = 0.0F, rowX = 0.0F, rowY = 0.0F;
      Node::absolutePosition(m_header, headerX, headerY);
      Node::absolutePosition(m_grid->focusArea(), rowX, rowY);
      leftGutter = std::max(0.0F, rowX - headerX);
      viewportWidth = m_grid->focusArea()->width();
    }
    const bool viewportChanged = m_leftGutter != leftGutter || m_viewportWidth != viewportWidth;
    m_leftGutter = leftGutter;
    m_viewportWidth = viewportWidth;
    const auto widths = listDisplayWidths(
        m_resizingColumn ? m_listColumns : m_preferredColumns, viewportWidth, m_visibleColumns, Style::spaceSm
    );
    const bool changed = widths.widths != m_listColumns.widths;
    m_listColumns = widths;
    if (!gestureActive())
      m_scrollOffset = std::min(m_scrollOffset, std::max(0.0F, contentWidth() - viewportWidth));
    const bool overflow = contentWidth() > viewportWidth + 0.5F;
    const bool scrollbarChanged = m_horizontalTrack && m_horizontalTrack->visible() != overflow;
    if (scrollbarChanged) {
      m_horizontalTrack->setVisible(overflow);
      m_horizontalTrack->setParticipatesInLayout(overflow);
    }
    for (const auto column : m_visibleColumns) {
      auto* header = m_headers[columnIndex(column)];
      header->setMinWidth(widths[column]);
      header->setMaxWidth(widths[column]);
      const float inset = columnTextInset(column);
      header->setPadding(0.0F, 0.0F, 0.0F, inset);
    }
    if (changed && m_grid)
      m_grid->notifyDataChanged();
    return changed || viewportChanged || scrollbarChanged;
  }

  void FileListHeaderController::arrangeAfterWidthChange(Renderer& renderer) {
    if (!m_header || m_gridMode)
      return;
    m_viewport->setSize(m_viewportWidth, m_header->height());
    m_viewport->setPosition(std::max(0.0F, m_leftGutter), 0.0F);
    const auto width = std::max(m_viewportWidth, contentWidth());
    if (m_grid && m_strip->x() != -m_scrollOffset) {
      m_grid->visitVisibleTiles([&](Node& tile, std::size_t) {
        if (auto* row = dynamic_cast<FileEntryRow*>(&tile))
          row->setColumnViewport(renderer, width, m_scrollOffset);
      });
    }
    m_strip->setSize(width, m_header->height());
    m_strip->setPosition(-m_scrollOffset, 0.0F);
    m_strip->layout(renderer);
    if (m_horizontalTrack && m_horizontalTrack->visible()) {
      m_horizontalScrollbar->setPosition(std::max(0.0F, m_leftGutter), 0.0F);
      m_horizontalScrollbar->setSize(m_viewportWidth, m_horizontalTrack->height());
      m_horizontalScrollbar->update(m_viewportWidth, contentWidth(), m_scrollOffset);
      m_horizontalScrollbar->layout(renderer);
    }
  }

  void FileListHeaderController::positionColumnMarkers() {
    if (!m_header || m_gridMode)
      return;
    for (const auto column : m_visibleColumns) {
      const auto bounds = listColumnBounds(column, m_listColumns, m_visibleColumns, Style::spaceSm);
      const auto edge = bounds.right + (column == m_visibleColumns.back() ? 0.0F : Style::spaceSm * 0.5F);
      m_markers[columnIndex(column)]->setPosition(edge - 1.0F, (m_header->height() - 20.0F) * 0.5F);
    }
  }

  void FileListHeaderController::saveWidths() {
    if (!m_settings.saveListColumnWidths(m_preferredColumns, m_searchView) && m_callbacks.onStatus)
      m_callbacks.onStatus("Unable to save file column widths");
  }

  void FileListHeaderController::requestFit(std::optional<FileColumn> column) {
    if (m_gridMode)
      return;
    m_pendingFit = column ? std::vector<FileColumn>{*column} : m_visibleColumns;
    if (m_callbacks.onRequestLayout)
      m_callbacks.onRequestLayout();
  }

  void FileListHeaderController::applyPendingFit(Renderer& renderer) {
    if (m_pendingFit.empty() || m_gridMode || !m_callbacks.measureColumn)
      return;
    const auto columns = std::exchange(m_pendingFit, {});
    if (m_preferredColumns[FileColumn::Name] == 0.0F)
      m_preferredColumns[FileColumn::Name] = m_listColumns[FileColumn::Name];
    for (const auto column : columns) {
      if (!std::ranges::contains(m_visibleColumns, column))
        continue;
      const auto heading = std::string(columnLabel(column)) + "  ↑";
      const auto headerWidth = renderer.measureText(heading, Style::fontSizeCaption).width;
      const float inset = columnTextInset(column);
      const auto width = std::max(headerWidth, m_callbacks.measureColumn(renderer, column)) + inset + Style::spaceSm;
      m_preferredColumns[column] = listColumnWidth(column, std::ceil(width));
    }
    saveWidths();
  }

  void FileListHeaderController::layoutVisibleMarkers(Renderer& renderer) {
    if (!m_header || m_gridMode)
      return;
    // Markers are positioned overlays: keep them out of Flex sizing, but still
    // lay out each Separator's internal gradient rectangles explicitly.
    for (auto* marker : m_markers)
      if (marker && marker->visible())
        marker->layout(renderer);
    if (m_dropMarker && m_dropMarker->visible())
      m_dropMarker->layout(renderer);
  }

  void FileListHeaderController::cancelGesture() {
    const auto resize = m_resizingColumn;
    m_dividerPress.reset();
    m_resizingColumn.reset();
    if (resize && m_markers[columnIndex(*resize)])
      m_markers[columnIndex(*resize)]->setColor(colorSpecFromRole(ColorRole::Primary, kResizeMarkerIdleOpacity));
    if (m_hoverBoundary && m_markers[columnIndex(*m_hoverBoundary)])
      m_markers[columnIndex(*m_hoverBoundary)]->setColor(
          colorSpecFromRole(ColorRole::Primary, kResizeMarkerIdleOpacity)
      );
    m_hoverBoundary.reset();
    if (m_columnDrag) {
      if (m_headers[columnIndex(m_columnDrag->column)])
        m_headers[columnIndex(m_columnDrag->column)]->setBorder(clearColorSpec(), 0.0F);
      if (m_dropMarker)
        m_dropMarker->setVisible(false);
      m_columnDrag.reset();
    }
    if (resize)
      applyColumnWidths();
    if (m_callbacks.onRequestLayout)
      m_callbacks.onRequestLayout();
  }

  bool FileListHeaderController::handlePointer(const PointerEvent& event, float x, float y) {
    if (m_gridMode
        || !m_header
        || m_visibleColumns.empty()
        || (m_callbacks.canHandlePointer && !m_callbacks.canHandlePointer()))
      return false;

    const auto pointerSerial = m_callbacks.pointerSerial ? m_callbacks.pointerSerial() : 0U;
    const bool leftButton = event.type == PointerEvent::Type::Button && event.button == BTN_LEFT;
    const bool motion = event.type == PointerEvent::Type::Motion;
    const bool leave = event.type == PointerEvent::Type::Leave;
    float hx = 0.0F, hy = 0.0F;
    Node::absolutePosition(m_header, hx, hy);
    const auto origin = stripOrigin();
    const float columnX = x - origin + m_scrollOffset;
    const bool inHeader =
        !leave && x >= origin && x < origin + availableWidth() && y >= hy && y < hy + m_header->height();
    const bool inHeaderArea = !leave && x >= hx && x < hx + m_header->width() && y >= hy && y < hy + m_header->height();
    if (m_resizingColumn) {
      const auto column = *m_resizingColumn;
      if (leave) {
        m_dividerPress.reset();
        cancelGesture();
        return true;
      }
      if (pointerSerial)
        m_wayland.setCursorShape(pointerSerial, WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_EW_RESIZE);
      if (motion) {
        if (std::abs(x - m_dragX) > 2.0F)
          m_dividerPress.reset();
        m_listColumns[column] = listColumnWidth(column, m_columnStart[column] + x - m_dragX);
        applyColumnWidths();
        if (m_grid)
          m_grid->notifyDataChanged();
        if (m_callbacks.onRequestLayout)
          m_callbacks.onRequestLayout();
        return true;
      }
      if (leftButton && !event.pressed) {
        if (m_dividerPress) {
          m_dividerClick = *m_dividerPress;
        } else if (m_listColumns[column] != m_columnStart[column]) {
          if (m_preferredColumns[FileColumn::Name] == 0.0F)
            m_preferredColumns[FileColumn::Name] = m_columnStart[FileColumn::Name];
          m_preferredColumns[column] = m_listColumns[column];
          saveWidths();
        }
        m_dividerPress.reset();
        cancelGesture();
        m_dispatcher.cancelPointerCapture();
        return true;
      }
      return true;
    }

    if (m_columnDrag) {
      auto& drag = *m_columnDrag;
      if (leave) {
        m_dispatcher.cancelPointerCapture();
        cancelGesture();
        return false;
      }
      if (leftButton && !event.pressed) {
        const auto column = drag.column;
        const auto destination = drag.destination;
        cancelGesture();
        if (!destination)
          return false; // A normal header click still goes through the button's sort action.
        if (inHeader && m_columnLayout.move(column, *destination)) {
          updateColumnHeaders();
          persistColumnLayout();
        }
        return true;
      }
      if (motion && (drag.destination || std::hypot(x - drag.x, y - drag.y) > 5.0F)) {
        if (!drag.destination) {
          m_dispatcher.cancelPointerCapture();
          m_dispatcher.pointerLeave();
          m_headers[columnIndex(drag.column)]->setBorder(
              colorSpecFromRole(ColorRole::Primary, kDraggedColumnBorderOpacity), 1.0F
          );
        }

        float edge = columnX;
        std::size_t destination = 0;
        for (const auto column : m_visibleColumns) {
          if (column == drag.column)
            continue;
          const auto bounds = listColumnBounds(column, m_listColumns, m_visibleColumns, Style::spaceSm);
          if (columnX < (bounds.left + bounds.right) * 0.5F) {
            edge = bounds.left - Style::spaceSm * 0.5F;
            break;
          }
          edge = bounds.right + Style::spaceSm * 0.5F;
          ++destination;
        }
        drag.destination = destination;
        m_dropMarker->setVisible(inHeader);
        m_dropMarker->setPosition(edge - 1.0F, (m_header->height() - 20.0F) * 0.5F);
        if (pointerSerial)
          m_wayland.setCursorShape(pointerSerial, WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_GRABBING);
        if (m_callbacks.onRequestLayout)
          m_callbacks.onRequestLayout();
        return true;
      }
      return false;
    }

    std::optional<FileColumn> boundary;
    if (inHeader) {
      for (const auto column : m_visibleColumns) {
        const auto bounds = listColumnBounds(column, m_listColumns, m_visibleColumns, Style::spaceSm);
        const auto edge = bounds.right + (column == m_visibleColumns.back() ? 0.0F : Style::spaceSm * 0.5F);
        if (std::abs(columnX - edge) <= 6.0F) {
          boundary = column;
          break;
        }
      }
    }
    if (inHeaderArea && event.type == PointerEvent::Type::Button && event.button == BTN_RIGHT && event.pressed) {
      if (m_callbacks.openHeaderMenu)
        m_callbacks.openHeaderMenu(boundary ? boundary : columnAt(x), x, y);
      return true;
    }
    if ((motion || leave || event.type == PointerEvent::Type::Enter) && m_hoverBoundary != boundary) {
      if (m_hoverBoundary)
        m_markers[columnIndex(*m_hoverBoundary)]->setColor(
            colorSpecFromRole(ColorRole::Primary, kResizeMarkerIdleOpacity)
        );
      m_hoverBoundary = boundary;
      if (boundary)
        m_markers[columnIndex(*boundary)]->setColor(colorSpecFromRole(ColorRole::Primary, kResizeMarkerHoveredOpacity));
      if (m_callbacks.onRequestRedraw)
        m_callbacks.onRequestRedraw();
    }
    if (boundary && leftButton && event.pressed) {
      m_dispatcher.cancelPointerCapture();
      if (m_dividerClick
          && m_dividerClick->column == *boundary
          && event.time - m_dividerClick->time <= 500U
          && std::abs(x - m_dividerClick->x) <= 6.0F) {
        requestFit(*boundary);
        m_dividerClick.reset();
        return true;
      }
      m_dividerClick.reset();
      m_dividerPress = DividerClick{*boundary, event.time, x};
      m_resizingColumn = boundary;
      m_dragX = x;
      m_columnStart = m_listColumns;
      m_markers[columnIndex(*boundary)]->setColor(colorSpecFromRole(ColorRole::Primary));
      return true;
    }
    if (boundary && (motion || event.type == PointerEvent::Type::Enter)) {
      m_dispatcher.pointerLeave();
      if (pointerSerial)
        m_wayland.setCursorShape(pointerSerial, WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_EW_RESIZE);
      return true;
    }
    if (leftButton && event.pressed) {
      m_dividerClick.reset();
      if (inHeader) {
        if (const auto column = columnAt(x))
          m_columnDrag = ColumnDrag{*column, x, y, std::nullopt};
      }
    }
    return false;
  }

  float FileListHeaderController::availableWidth() const { return m_viewportWidth; }

} // namespace filemanager
