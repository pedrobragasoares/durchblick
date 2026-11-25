/*************************************************************************
 * This file is part of durchblick
 * git.vrsal.xyz/alex/durchblick
 * Copyright 2023 univrsal <uni@vrsal.xyz>.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, version 2 of the License.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *************************************************************************/

#include "layout.hpp"
#include "util/obs_settings.hpp"
#include "config.hpp"
#include "items/preview_program_item.hpp"
#include "items/scene_item.hpp"
#include "items/source_item.hpp"
#include "ui/durchblick.hpp"
#include "util/util.h"
#include <QApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <obs-frontend-api.h>
#include <util/config-file.h>

void Layout::FillEmptyCells()
{
    // Make sure that every cell has a placeholder
    std::vector<LayoutItem::Cell> empty;
    for (int x = 0; x < m_cols; x++) {
        for (int y = 0; y < m_rows; y++) {
            LayoutItem::Cell c;
            c.col = x;
            c.row = y;
            bool isFree = true;
            for (auto const& item : m_layout_items) {
                if (c.Overlaps(item->m_cell)) {
                    isFree = false;
                    break;
                }
            }

            if (isFree)
                empty.emplace_back(c);
        }
    }

    for (auto const& c : empty) {
        auto* Item = new PlaceholderItem(this, c.col, c.row);
        Item->Update(m_cfg);
        m_layout_items.emplace_back(Item);
    }
}

LayoutItem::Cell Layout::GetSelectedArea()
{
    LayoutItem::Cell target = m_hovered_cell;

    if (m_dragging) {
        int tx, ty, cx, cy;
        GetSelection(tx, ty, cx, cy);
        target.col = tx;
        target.row = ty;
        target.w = cx;
        target.h = cy;
    }
    return target;
}

void Layout::ClearSelection()
{
    auto target = GetSelectedArea();
    m_layout_mutex.lock();
    FreeSpace(target);
    FillEmptyCells();
    m_layout_mutex.unlock();
    Config::Save();
}

void Layout::ResolveSources()
{
    std::lock_guard<std::mutex> lock(m_layout_mutex);
    for (auto& item : m_layout_items) {
        if (auto* si = dynamic_cast<SourceItem*>(item.get()))
            si->ResolveSource();
    }
}

void Layout::FillSelectionWithScenes()
{
    {
        std::lock_guard<std::mutex> lock(m_layout_mutex);
        struct obs_frontend_source_list scenes = {};
        obs_frontend_get_scenes(&scenes);

        auto target = GetSelectedArea();
        auto cells_to_fill = target.w * target.h;
        std::vector<std::string> existing_scenes;
        FreeSpace(target);

        // Get all scenes that are already in the layout
        for (auto const& item : m_layout_items) {
            auto* si = dynamic_cast<SceneItem*>(item.get());
            if (si && si->GetSource().Get())
                existing_scenes.emplace_back(obs_source_get_name(si->GetSource()));
        }

        auto* ItemEntry = Registry::GetEntryById("SceneItem");
        if (!ItemEntry)
            return;

        int col {}, row {};
        for (int i = 0; i < int(scenes.sources.num) && cells_to_fill > 0; i++) {
            auto* src = scenes.sources.array[i];

            // Don't add scenes that shouldn't be shown in the normal multiview
            OBSDataAutoRelease data = obs_source_get_private_settings(src);
            obs_data_set_default_bool(data, "show_in_multiview", true);
            if (!obs_data_get_bool(data, "show_in_multiview"))
                continue;

            std::string n(obs_source_get_name(src));
            if (m_hidden_scenes.contains(utf8_to_qt(n.c_str())))
                continue;
            auto exists = std::find_if(existing_scenes.begin(), existing_scenes.end(), [n](std::string const& name) {
                return n == name;
            });
            if (exists != existing_scenes.end())
                continue;

            LayoutItem::Cell c;
            c.col = target.col + col;
            c.row = target.row + row;

            col++;
            if (col >= target.w) {
                col = 0;
                row++;
            }

            auto* Item = new SceneItem(this, c.col, c.row);
            Item->Update(m_cfg);
            Item->SetSource(src);
            FreeSpace(c);
            m_layout_items.emplace_back(Item);
            cells_to_fill--;
        }
        FillEmptyCells();
        obs_frontend_source_list_free(&scenes);
    }
    Config::Save();
}

Layout::Layout(Durchblick* parent, int cols, int rows)
    : QObject(parent)
    , m_cols(cols)
    , m_rows(rows)
    , m_durchblick(parent)
{
}

Layout::~Layout()
{
}

LayoutItem::MouseData Layout::MakeMouseData(QMouseEvent* e)
{
    LayoutItem::MouseData d(
        int((e->pos().x() - m_cfg.x) / m_cfg.scale),
        int((e->pos().y() - m_cfg.y) / m_cfg.scale),
        e->modifiers(),
        e->buttons(),
        e->type());

    // Apply the display scaling for every event, not just mouse moves,
    // otherwise clicks land on the wrong cell with Windows scaling > 100%
    auto* handle = m_durchblick->windowHandle();
    auto* screen = handle ? handle->screen() : nullptr;
    if (screen) {
        d.x *= screen->devicePixelRatio();
        d.y *= screen->devicePixelRatio();
    }
    return d;
}

LayoutItem::Cell Layout::CellAt(LayoutItem::MouseData const& d) const
{
    LayoutItem::Cell c;
    if (m_cfg.cell_width <= 0 || m_cfg.cell_height <= 0)
        return c;
    c.col = qBound(0, int(d.x / m_cfg.cell_width), m_cols - 1);
    c.row = qBound(0, int(d.y / m_cfg.cell_height), m_rows - 1);
    return c;
}

LayoutItem* Layout::ItemAt(int col, int row)
{
    LayoutItem::Cell c;
    c.col = col;
    c.row = row;
    for (auto& item : m_layout_items) {
        if (item->m_cell.Overlaps(c))
            return item.get();
    }
    return nullptr;
}

void Layout::MouseMoved(QMouseEvent* e)
{
    auto d = MakeMouseData(e);

    // Left button held on a cell: start moving it once the mouse traveled far enough
    if (m_lmb_pressed && !m_locked && (e->buttons() & Qt::LeftButton)) {
        if (!m_moving && (e->pos() - m_press_pos).manhattanLength() >= QApplication::startDragDistance()) {
            std::lock_guard<std::mutex> lock(m_layout_mutex);
            auto press_cell = m_move_source;
            auto* item = ItemAt(press_cell.col, press_cell.row);
            if (item && !dynamic_cast<PlaceholderItem*>(item)) {
                m_moving = true;
                m_move_source = item->m_cell;
                for (auto& Item : m_layout_items)
                    Item->CancelClick();
            } else {
                m_lmb_pressed = false;
            }
        }
        if (m_moving) {
            auto c = CellAt(d);
            m_move_target = c;
            {
                // Show the target with the size of the moved cell
                std::lock_guard<std::mutex> lock(m_layout_mutex);
                auto* target = ItemAt(c.col, c.row);
                if (target && target->m_cell.w == m_move_source.w && target->m_cell.h == m_move_source.h)
                    m_move_target = target->m_cell;
                else {
                    m_move_target.w = m_move_source.w;
                    m_move_target.h = m_move_source.h;
                }
            }
            for (auto& Item : m_layout_items)
                Item->MouseEvent(d, m_cfg);
            return;
        }
    }

    LayoutItem::Cell pos;
    bool anything_hovered = false;
    for (auto& Item : m_layout_items) {
        Item->MouseEvent(d, m_cfg);
        if (Item->Hovered()) {
            pos = Item->m_hovered_cell;
            anything_hovered = true;
        }
    }
    m_hovered_cell = pos;
    if (anything_hovered && e->buttons() & Qt::RightButton) {
        // Dragging
        m_selection_end = pos;
        m_dragging = true;
    } else {
        m_dragging = false;
    }

    if (!m_dragging) {
        m_selection_end.clear();
        m_selection_start.clear();
    }
}

void Layout::MousePressed(QMouseEvent* e)
{
    auto d = MakeMouseData(e);
    for (auto& Item : m_layout_items)
        Item->MouseEvent(d, m_cfg);
    if (e->button() == Qt::RightButton) {
        m_selection_start = m_hovered_cell;
    } else {
        m_selection_end.clear();
        m_selection_start.clear();
        m_dragging = false;
    }

    if (e->button() == Qt::LeftButton && !m_locked) {
        m_lmb_pressed = true;
        m_moving = false;
        m_press_pos = e->pos();
        m_move_source = CellAt(d); // the exact cell, resolved to the item once the drag starts
    }
}

void Layout::MouseReleased(QMouseEvent* e)
{
    auto d = MakeMouseData(e);

    if (m_moving && e->button() == Qt::LeftButton) {
        bool changed;
        {
            std::lock_guard<std::mutex> lock(m_layout_mutex);
            changed = MoveItem(m_move_source, CellAt(d));
            for (auto& Item : m_layout_items)
                Item->CancelClick();
        }
        m_moving = false;
        m_lmb_pressed = false;
        if (changed)
            Config::Save();
        return; // A drag is never a click
    }
    if (e->button() == Qt::LeftButton) {
        m_lmb_pressed = false;
        m_moving = false;
    }

    for (auto& Item : m_layout_items)
        Item->MouseEvent(d, m_cfg);
    m_dragging = false;
}

void Layout::MouseDoubleClicked(QMouseEvent* e)
{
    auto d = MakeMouseData(e);
    d.double_click = true;
    for (auto& Item : m_layout_items)
        Item->MouseEvent(d, m_cfg);
}

bool Layout::MoveItem(LayoutItem::Cell const& from, LayoutItem::Cell const& to)
{
    auto* src = ItemAt(from.col, from.row);
    auto* dst = ItemAt(to.col, to.row);
    if (!src || !dst || src == dst)
        return false;

    // Same size: swap the two cells (keeps each cell's settings with it)
    if (src->m_cell.w == dst->m_cell.w && src->m_cell.h == dst->m_cell.h) {
        std::swap(src->m_cell, dst->m_cell);
        src->Update(m_cfg);
        dst->Update(m_cfg);
        return true;
    }

    // Moving onto empty space: only if every cell it would cover is empty
    LayoutItem::Cell target = src->m_cell;
    target.col = to.col;
    target.row = to.row;
    if (target.right() <= m_cols && target.bottom() <= m_rows) {
        bool free = true;
        for (auto& item : m_layout_items) {
            if (item.get() != src && item->m_cell.Overlaps(target) && !dynamic_cast<PlaceholderItem*>(item.get())) {
                free = false;
                break;
            }
        }
        if (free) {
            // Remove the empty cells under the new position
            auto it = std::remove_if(m_layout_items.begin(), m_layout_items.end(), [&](std::unique_ptr<LayoutItem> const& item) {
                return item.get() != src && item->m_cell.Overlaps(target);
            });
            m_layout_items.erase(it, m_layout_items.end());
            src->m_cell = target;
            src->Update(m_cfg);
            FillEmptyCells();
            return true;
        }
    }

    // Different sizes: just exchange what the two cells show
    auto* a = dynamic_cast<SourceItem*>(src);
    auto* b = dynamic_cast<SourceItem*>(dst);
    if (a && b) {
        auto ra = a->GetSourceRef();
        auto rb = b->GetSourceRef();
        a->ApplySourceRef(rb);
        b->ApplySourceRef(ra);
        return true;
    }
    return false;
}

void Layout::DrawOutline(LayoutItem::Cell const& c, uint32_t color)
{
    int tx = c.col, ty = c.row, cx = c.w, cy = c.h;
    // Top
    LayoutItem::DrawBox(tx * m_cfg.cell_width, ty * m_cfg.cell_height - 1, cx * m_cfg.cell_width - 1, m_cfg.border + 1, color);
    // Bottom
    LayoutItem::DrawBox(tx * m_cfg.cell_width, (ty + cy) * m_cfg.cell_height - m_cfg.border - 2, cx * m_cfg.cell_width - 1, m_cfg.border + 2, color);
    // Left
    LayoutItem::DrawBox(tx * m_cfg.cell_width, ty * m_cfg.cell_height, m_cfg.border, cy * m_cfg.cell_height - 1, color);
    // Right
    LayoutItem::DrawBox((tx + cx) * m_cfg.cell_width - m_cfg.border - 2, ty * m_cfg.cell_height, m_cfg.border + 1, cy * m_cfg.cell_height - 1, color);
}

// ---------------------------------------------------------------------------
// Hiding scenes: the following scene cells move up one position

static QStringList ObsSceneNames()
{
    QStringList names;
    struct obs_frontend_source_list scenes = {};
    obs_frontend_get_scenes(&scenes);
    for (size_t i = 0; i < scenes.sources.num; i++)
        names.append(utf8_to_qt(obs_source_get_name(scenes.sources.array[i])));
    obs_frontend_source_list_free(&scenes);
    return names;
}

static bool ShownInMultiview(obs_source_t* src)
{
    OBSDataAutoRelease data = obs_source_get_private_settings(src);
    obs_data_set_default_bool(data, "show_in_multiview", true);
    return obs_data_get_bool(data, "show_in_multiview");
}

std::vector<SceneItem*> Layout::SceneItemsInOrder()
{
    std::vector<SceneItem*> result;
    for (auto& item : m_layout_items) {
        if (auto* si = dynamic_cast<SceneItem*>(item.get()))
            result.push_back(si);
    }
    // Reading order: left to right, top to bottom
    std::sort(result.begin(), result.end(), [](SceneItem* a, SceneItem* b) {
        if (a->m_cell.row != b->m_cell.row)
            return a->m_cell.row < b->m_cell.row;
        return a->m_cell.col < b->m_cell.col;
    });
    return result;
}

void Layout::HideScene(QString const& name)
{
    if (name.isEmpty())
        return;
    {
        std::lock_guard<std::mutex> lock(m_layout_mutex);
        if (!m_hidden_scenes.contains(name))
            m_hidden_scenes.append(name);

        auto items = SceneItemsInOrder();
        std::vector<SourceItem::SourceRef> refs;
        for (auto* si : items)
            refs.push_back(si->GetSourceRef());

        auto it = std::find_if(refs.begin(), refs.end(), [&](SourceItem::SourceRef const& r) { return r.name == name; });
        if (it == refs.end()) {
            // Not shown here, just remember it as hidden
        } else {
            refs.erase(it);

            // Fill the freed slot at the end with the next scene that isn't shown yet
            QStringList shown;
            for (auto const& r : refs)
                shown.append(r.name);
            for (auto const& scene_name : ObsSceneNames()) {
                if (shown.contains(scene_name) || m_hidden_scenes.contains(scene_name))
                    continue;
                OBSSourceAutoRelease src = obs_get_source_by_name(qt_to_utf8(scene_name));
                if (!src || !ShownInMultiview(src))
                    continue;
                refs.push_back({ OBSSource(src.Get()), scene_name });
                break;
            }

            for (size_t i = 0; i < items.size(); i++) {
                if (i < refs.size()) {
                    items[i]->ApplySourceRef(refs[i]);
                } else {
                    // Nothing left to show, turn the last cells into empty ones
                    FreeSpace(items[i]->m_cell);
                }
            }
            FillEmptyCells();
        }
    }
    Config::Save();
}

void Layout::UnhideScene(QString const& name)
{
    {
        std::lock_guard<std::mutex> lock(m_layout_mutex);
        m_hidden_scenes.removeAll(name);

        OBSSourceAutoRelease src = obs_get_source_by_name(qt_to_utf8(name));
        if (src && obs_scene_from_source(src)) {
            auto items = SceneItemsInOrder();
            std::vector<SourceItem::SourceRef> refs;
            bool already_shown = false;
            for (auto* si : items) {
                refs.push_back(si->GetSourceRef());
                already_shown |= si->SourceName() == name;
            }

            if (!already_shown) {
                // Put it back where it belongs according to the OBS scene order
                auto order = ObsSceneNames();
                auto pos = order.indexOf(name);
                size_t ins = refs.size();
                for (size_t i = 0; i < refs.size(); i++) {
                    auto idx = order.indexOf(refs[i].name);
                    if (refs[i].IsEmpty() || idx < 0 || idx > pos) {
                        ins = i;
                        break;
                    }
                }
                refs.insert(refs.begin() + ins, { OBSSource(src.Get()), name });

                // Use up an empty scene cell after it instead of pushing the last scene out
                for (size_t j = ins + 1; j < refs.size(); j++) {
                    if (refs[j].IsEmpty()) {
                        refs.erase(refs.begin() + j);
                        break;
                    }
                }

                // Still one too many: add a scene cell in the first free spot after the last one
                if (refs.size() > items.size()) {
                    LayoutItem* free_cell = nullptr;
                    int last_row = items.empty() ? -1 : items.back()->m_cell.row;
                    int last_col = items.empty() ? -1 : items.back()->m_cell.col;
                    for (int y = 0; y < m_rows && !free_cell; y++) {
                        for (int x = 0; x < m_cols && !free_cell; x++) {
                            if (y < last_row || (y == last_row && x <= last_col))
                                continue;
                            auto* item = ItemAt(x, y);
                            if (item && dynamic_cast<PlaceholderItem*>(item))
                                free_cell = item;
                        }
                    }
                    if (free_cell) {
                        auto c = free_cell->m_cell;
                        FreeSpace(c);
                        auto* new_item = new SceneItem(this, c.col, c.row);
                        new_item->Update(m_cfg);
                        m_layout_items.emplace_back(new_item);
                        items = SceneItemsInOrder();
                    }
                }

                for (size_t i = 0; i < items.size() && i < refs.size(); i++)
                    items[i]->ApplySourceRef(refs[i]);
            }
        }
    }
    Config::Save();
}

void Layout::UnhideAllScenes()
{
    // Restore in OBS scene order so each one lands in the right spot
    auto order = ObsSceneNames();
    auto hidden = m_hidden_scenes;
    std::sort(hidden.begin(), hidden.end(), [&](QString const& a, QString const& b) {
        return order.indexOf(a) < order.indexOf(b);
    });
    for (auto const& name : hidden)
        UnhideScene(name);
    m_hidden_scenes.clear();
    Config::Save();
}

void Layout::HandleContextMenu(QMouseEvent*, QMenu& m)
{
    if (m_locked) {
        m.addAction(T_MENU_UNLOCK, this, SLOT(Unlock()));
    } else {

        // Keep drawing the selection if it wasn't reset
        if (!m_selection_end.empty())
            m_dragging = true;

        m.addAction(T_MENU_CONFIGURATION, this, SLOT(ShowLayoutConfigDialog()));
        m.addAction(T_MENU_LOCK, this, SLOT(Lock()));
        std::lock_guard<std::mutex> lock(m_layout_mutex);

        if (!m_hidden_scenes.isEmpty()) {
            auto* hidden_menu = m.addMenu(QString("%1 (%2)").arg(T_MENU_HIDDEN_SCENES).arg(m_hidden_scenes.size()));
            for (auto const& name : std::as_const(m_hidden_scenes))
                hidden_menu->addAction(name, this, [this, name] { UnhideScene(name); });
            hidden_menu->addSeparator();
            hidden_menu->addAction(T_MENU_SHOW_ALL_HIDDEN, this, [this] { UnhideAllScenes(); });
        }

        for (auto& Item : m_layout_items) {
            if (Item->Hovered()) {
                auto* sub_menu = m.addMenu(T_MENU_QUICK_ACTIONS);
                sub_menu->addAction(T_MENU_FILL_ACTION, this, SLOT(FillSelectionWithScenes()));
                sub_menu->addAction(T_MENU_CLEAR_ACTION, this, SLOT(ClearSelection()));
                if (auto* si = dynamic_cast<SceneItem*>(Item.get())) {
                    auto name = si->SourceName();
                    if (!name.isEmpty())
                        m.addAction(T_MENU_HIDE_SCENE, this, [this, name] { HideScene(name); });
                }
                m.addAction(T_MENU_SET_WIDGET, this, SLOT(ShowSetWidgetDialog()));
                m.addSeparator();
                Item->ContextMenu(m);
                break;
            }
        }
    }
}

void Layout::FreeSpace(LayoutItem::Cell const& c)
{
    auto it = std::remove_if(m_layout_items.begin(), m_layout_items.end(), [c](std::unique_ptr<LayoutItem> const& item) {
        auto result = !item || c.Overlaps(item->m_cell);
        return result;
    });
    m_layout_items.erase(it, m_layout_items.end());
}

void Layout::AddWidget(Registry::ItemRegistry::Entry const& entry, const LayoutItem::Cell& c, QWidget* custom_widget)
{
    auto* Item = entry.construct(this, c.col, c.row, c.w, c.h);
    Item->LoadConfigFromWidget(custom_widget);
    Item->Update(m_cfg);

    m_layout_mutex.lock();
    FreeSpace(c);
    m_layout_items.emplace_back(Item);
    FillEmptyCells();
    m_layout_mutex.unlock();

    Config::Save();
}

void Layout::AddWidget(Registry::ItemRegistry::Entry const& entry, QWidget* custom_widget)
{
    AddWidget(entry, GetSelectedArea(), custom_widget);
}

void Layout::SetRegion(float bx, float by, float cx, float cy)
{
    float vX = int(m_cfg.x + bx * m_cfg.scale);
    float vY = int(m_cfg.y + by * m_cfg.scale);
    float vCX = int(cx * m_cfg.scale);
    float vCY = int(cy * m_cfg.scale);

    float oL = bx;
    float oT = by;
    float oR = (bx + cx);
    float oB = (by + cy);
    StartRegion(vX, vY, vCX, vCY, oL, oR, oT, oB);
}

void Layout::Render(int, int, uint32_t, uint32_t)
{
    if (!m_durchblick->HasSize()) // We need at least one refresh/resize to be sure that we have all necessary data for rendering
        return;
    // Define the whole usable region for the multiview
    StartRegion(m_cfg.x, m_cfg.y, m_cfg.cx * m_cfg.scale, m_cfg.cy * m_cfg.scale, 0.0f, m_cfg.cx,
        0.0f, m_cfg.cy);
    LayoutItem::DrawBox(m_cfg.cx, m_cfg.cy, COLOR_BORDER_GRAY);

    m_layout_mutex.lock();
    for (auto& Item : m_layout_items) {
        // Change region to item dimensions
        gs_matrix_push();
        gs_matrix_translate3f(Item->m_rel_left, Item->m_rel_top, 0);

        SetRegion(Item->m_rel_left, Item->m_rel_top, Item->m_width, Item->m_height);

        LayoutItem::DrawBox(0, 0, m_cfg.cell_width * Item->m_width, m_cfg.cell_height * Item->m_height, Item->GetFillColor());

        EndRegion();
        gs_matrix_pop();

        gs_matrix_push();
        gs_matrix_translate3f(Item->m_rel_left + m_cfg.border, Item->m_rel_top + m_cfg.border, 0);
        SetRegion(Item->m_rel_left + m_cfg.border, Item->m_rel_top + m_cfg.border, Item->m_inner_width, Item->m_inner_height);
        Item->Render(m_cfg);
        EndRegion();
        gs_matrix_pop();
    }
    m_layout_mutex.unlock();

    if (m_moving) {
        DrawOutline(m_move_source, COLOR_SELECTION_CYAN);
        DrawOutline(m_move_target, COLOR_PREVIEW_INDICATOR);
    }

    if (m_dragging) {
        int tx, ty, cx, cy;
        GetSelection(tx, ty, cx, cy);
        // Draw Selection rectangle

        // Top
        LayoutItem::DrawBox(tx * m_cfg.cell_width, ty * m_cfg.cell_height - 1, cx * m_cfg.cell_width - 1, m_cfg.border + 1, COLOR_SELECTION_CYAN);

        // Bottom
        LayoutItem::DrawBox(tx * m_cfg.cell_width, (ty + cy) * m_cfg.cell_height - m_cfg.border - 2, cx * m_cfg.cell_width - 1, m_cfg.border + 2, COLOR_SELECTION_CYAN);

        // Left
        LayoutItem::DrawBox(tx * m_cfg.cell_width, ty * m_cfg.cell_height, m_cfg.border, cy * m_cfg.cell_height - 1, COLOR_SELECTION_CYAN);

        // Right
        LayoutItem::DrawBox((tx + cx) * m_cfg.cell_width - m_cfg.border - 2, ty * m_cfg.cell_height, m_cfg.border + 1, cy * m_cfg.cell_height - 1, COLOR_SELECTION_CYAN);
    }
    EndRegion();
}

void Layout::Resize(int target_cx, int target_cy, int cx, int cy)
{
    // We calculate most layout values only on resize here
    m_cfg.canvas_width = target_cx;
    m_cfg.canvas_height = target_cy;

    float ar = float(target_cx) / float(target_cy);

    // TODO: do height first and then calculate target_cx based on that?
    m_cfg.cell_width = float(target_cx) / m_cols;
    m_cfg.cell_height = m_cfg.cell_width / ar;

    target_cy = m_cfg.cell_height * m_rows;

    m_cfg.cx = target_cx;
    m_cfg.cy = target_cy;

    GetScaleAndCenterPos(target_cx, target_cy, cx, cy, m_cfg.x, m_cfg.y, m_cfg.scale);

    m_layout_mutex.lock();
    for (auto& Item : m_layout_items)
        Item->Update(m_cfg);
    m_layout_mutex.unlock();
}

void Layout::RefreshGrid()
{
    auto target_cx = m_cfg.canvas_width;
    auto target_cy = m_cfg.canvas_height;

    float ar = float(target_cx) / float(target_cy);
    m_cfg.cell_width = float(m_cfg.canvas_width) / m_cols;
    m_cfg.cell_height = m_cfg.cell_width / ar;

    target_cy = m_cfg.cell_height * m_rows;

    m_cfg.cx = target_cx;
    m_cfg.cy = target_cy;

    auto* db = static_cast<Durchblick*>(parent());
    auto s = db->size() * db->devicePixelRatioF();
    GetScaleAndCenterPos(target_cx, target_cy, s.width(), s.height(), m_cfg.x, m_cfg.y, m_cfg.scale);

    // Delete any cells that don't fit on the screen anymore
    m_layout_mutex.lock();
    auto it = std::remove_if(m_layout_items.begin(), m_layout_items.end(), [this](std::unique_ptr<LayoutItem> const& item) {
        return item->m_cell.right() >= m_cols + 1 || item->m_cell.bottom() >= m_rows + 1;
    });
    m_layout_items.erase(it, m_layout_items.end());
    FillEmptyCells();

    for (auto& Item : m_layout_items)
        Item->Update(m_cfg);
    m_layout_mutex.unlock();
}

void Layout::CreateDefaultLayout()
{
    m_layout_mutex.lock();
    auto* preview = new PreviewProgramItem(this, 0, 0, 2, 2);
    auto* program = new PreviewProgramItem(this, 2, 0, 2, 2);
    program->SetIsProgram(true);
    program->SetLabel(true);
    program->CreateLabel();
    program->Update(m_cfg);
    preview->SetSafeBorders(true);
    preview->SetLabel(true);
    preview->CreateLabel();
    preview->Update(m_cfg);
    m_layout_items.emplace_back(preview);
    m_layout_items.emplace_back(program);
    m_cols = 4;
    m_rows = 4;

    struct obs_frontend_source_list scenes = {};

    obs_frontend_get_scenes(&scenes);
    for (int i = 0; i < 8; i++) {
        if (i >= int(scenes.sources.num)) {
            auto* Item = new PlaceholderItem(this, i % 4, i > 3 ? 3 : 2);
            Item->Update(m_cfg);
            m_layout_items.emplace_back(Item);
        } else {
            auto* item = new SceneItem(this, i % 4, i > 3 ? 3 : 2);
            item->SetLabel(true);
            item->SetSource(scenes.sources.array[i]);
            item->Update(m_cfg);
            m_layout_items.emplace_back(item);
        }
    }
    m_layout_mutex.unlock();
    obs_frontend_source_list_free(&scenes);

    // Automatically set settings to user default
    m_durchblick->SetHideFromDisplayCapture(ObsSettings::GetBasicWindowBool("HideOBSWindowsFromCapture"));

    m_durchblick->SetHideCursor(ObsSettings::GetBasicWindowBool("HideProjectorCursor"));
    m_durchblick->SetIsAlwaysOnTop(ObsSettings::GetBasicWindowBool("ProjectorAlwaysOnTop"));
}

void Layout::Load(QJsonObject const& obj)
{
    Clear();

    m_layout_mutex.lock();
    m_cols = obj["cols"].toInt(4);
    m_rows = obj["rows"].toInt(4);
    m_locked = obj["locked"].toBool(false);
    m_hidden_scenes.clear();
    for (auto const& v : obj["hidden_scenes"].toArray())
        m_hidden_scenes.append(v.toString());
    auto items = obj["items"].toArray();

    for (auto const& item : std::as_const(items)) {
        auto* new_item = Registry::MakeItem(this, item.toObject());
        if (new_item) {
            new_item->Update(m_cfg);
            m_layout_items.emplace_back(new_item);
        } else {
            QJsonDocument doc;
            doc.setObject(item.toObject());
            berr("Failed to instanciate widget '%s'", qt_to_utf8(item.toObject()["id"].toString()));
            berr("Widget JSON: %s", qt_to_utf8(QString(doc.toJson())));
        }
    }
    m_layout_mutex.unlock();
    if (IsEmpty())
        CreateDefaultLayout();

    RefreshGrid();
}

void Layout::Save(QJsonObject& obj)
{
    std::lock_guard<std::mutex> lock(m_layout_mutex);
    QJsonArray items;
    obj["cols"] = m_cols;
    obj["rows"] = m_rows;
    obj["locked"] = m_locked;
    QJsonArray hidden;
    for (auto const& name : std::as_const(m_hidden_scenes))
        hidden.append(name);
    obj["hidden_scenes"] = hidden;
    for (auto const& Item : m_layout_items) {
        QJsonObject obj;
        Item->WriteToJson(obj);
        items.append(obj);
    }
    obj["items"] = items;
}

void Layout::DeleteLayout()
{
    m_layout_mutex.lock();
    m_layout_items.clear();
    m_layout_mutex.unlock();
}

void Layout::ResetHover()
{
    if (m_hovered_cell.col > -1) {
        m_hovered_cell.col = -1;
        m_hovered_cell.row = -1;
        auto pos = m_durchblick->mapFromGlobal(QCursor::pos());
        for (auto const& i : m_layout_items) {
            i->IsMouseOver(pos.x(), pos.y());
        }
    }
}
