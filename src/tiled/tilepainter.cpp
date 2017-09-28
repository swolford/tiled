/*
 * tilepainter.cpp
 * Copyright 2009-2011, Thorbjørn Lindeijer <thorbjorn@lindeijer.nl>
 * Copyright 2009, Jeff Bland <jksb@member.fsf.org>
 *
 * This file is part of Tiled.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include "tilepainter.h"

#include "mapdocument.h"
#include "map.h"

#include <QQueue>

using namespace Tiled;
using namespace Tiled::Internal;

namespace {

class TileLayerChangeWatcher
{
public:
    TileLayerChangeWatcher(MapDocument *mapDocument, TileLayer *layer)
        : mMapDocument(mapDocument)
        , mTileLayer(layer)
        , mDrawMargins(layer->drawMargins())
        , mBounds(layer->bounds())
    {
    }

    ~TileLayerChangeWatcher()
    {
        if (mTileLayer->map() != mMapDocument->map())
            return;

        MapDocument::TileLayerChangeFlags flags;

        if (mTileLayer->drawMargins() != mDrawMargins)
            flags |= MapDocument::LayerDrawMarginsChanged;
        if (mTileLayer->bounds() != mBounds)
            flags |= MapDocument::LayerBoundsChanged;

        if (flags)
            emit mMapDocument->tileLayerChanged(mTileLayer, flags);
    }

private:
    MapDocument *mMapDocument;
    TileLayer *mTileLayer;
    const QMargins mDrawMargins;
    const QRect mBounds;
};

} // anonymous namespace


TilePainter::TilePainter(MapDocument *mapDocument, TileLayer *tileLayer)
    : mMapDocument(mapDocument)
    , mTileLayer(tileLayer)
{
}

Cell TilePainter::cellAt(int x, int y) const
{
    const int layerX = x - mTileLayer->x();
    const int layerY = y - mTileLayer->y();

    return mTileLayer->cellAt(layerX, layerY);
}

void TilePainter::setCell(int x, int y, const Cell &cell)
{
    const QRegion &selection = mMapDocument->selectedArea();
    if (!(selection.isEmpty() || selection.contains(QPoint(x, y))))
        return;

    const int layerX = x - mTileLayer->x();
    const int layerY = y - mTileLayer->y();

    if (!mTileLayer->contains(layerX, layerY) && !mMapDocument->map()->infinite())
        return;

    TileLayerChangeWatcher watcher(mMapDocument, mTileLayer);
    mTileLayer->setCell(layerX, layerY, cell);
    emit mMapDocument->regionChanged(QRegion(x, y, 1, 1), mTileLayer);
}

void TilePainter::setCells(int x, int y,
                           TileLayer *tileLayer,
                           const QRegion &mask)
{
    QRegion region = paintableRegion(x, y,
                                     tileLayer->width(),
                                     tileLayer->height());
    region &= mask;

    if (region.isEmpty())
        return;

    TileLayerChangeWatcher watcher(mMapDocument, mTileLayer);
    mTileLayer->setCells(x - mTileLayer->x(),
                         y - mTileLayer->y(),
                         tileLayer,
                         region.translated(-mTileLayer->position()));

    emit mMapDocument->regionChanged(region, mTileLayer);
}

void TilePainter::drawCells(int x, int y, TileLayer *tileLayer)
{
    const QRegion region = paintableRegion(x, y,
                                           tileLayer->width(),
                                           tileLayer->height());
    if (region.isEmpty())
        return;

    TileLayerChangeWatcher watcher(mMapDocument, mTileLayer);

    for (const QRect &rect : region.rects()) {
        for (int _y = rect.top(); _y <= rect.bottom(); ++_y) {
            for (int _x = rect.left(); _x <= rect.right(); ++_x) {
                const Cell &cell = tileLayer->cellAt(_x - x, _y - y);
                if (cell.isEmpty())
                    continue;

                mTileLayer->setCell(_x - mTileLayer->x(),
                                    _y - mTileLayer->y(),
                                    cell);
            }
        }
    }

    emit mMapDocument->regionChanged(region, mTileLayer);
}

void TilePainter::drawStamp(const TileLayer *stamp,
                            const QRegion &drawRegion)
{
    Q_ASSERT(stamp);
    if (stamp->bounds().isEmpty())
        return;

    const QRegion region = paintableRegion(drawRegion);
    if (region.isEmpty())
        return;

    TileLayerChangeWatcher watcher(mMapDocument, mTileLayer);

    const int w = stamp->width();
    const int h = stamp->height();
    const QRect regionBounds = region.boundingRect();

    for (const QRect &rect : region.rects()) {
        for (int _y = rect.top(); _y <= rect.bottom(); ++_y) {
            for (int _x = rect.left(); _x <= rect.right(); ++_x) {
                const int stampX = (_x - regionBounds.left()) % w;
                const int stampY = (_y - regionBounds.top()) % h;
                const Cell &cell = stamp->cellAt(stampX, stampY);
                if (cell.isEmpty())
                    continue;

                mTileLayer->setCell(_x - mTileLayer->x(),
                                    _y - mTileLayer->y(),
                                    cell);
            }
        }
    }

    emit mMapDocument->regionChanged(region, mTileLayer);
}

void TilePainter::erase(const QRegion &region)
{
    const QRegion paintable = paintableRegion(region);
    if (paintable.isEmpty())
        return;

    mTileLayer->erase(paintable.translated(-mTileLayer->position()));
    emit mMapDocument->regionChanged(paintable, mTileLayer);
}

static QRegion fillRegion(const TileLayer *layer, QPoint fillOrigin,
                          Map::Orientation orientation,
                          Map::StaggerAxis staggerAxis,
                          Map::StaggerIndex staggerIndex)
{
    // Create that region that will hold the fill
    QRegion fillRegion;
    QRect bounds = layer->map()->infinite() ? layer->bounds() : layer->rect();

    // Silently quit if parameters are unsatisfactory
    if (!bounds.contains(fillOrigin))
        return fillRegion;

    bounds.translate(-layer->position());

    // Cache cell that we will match other cells against
    const Cell matchCell = layer->cellAt(fillOrigin);

    int startX = bounds.left();
    int startY = bounds.top();
    int endX = bounds.right() + 1;
    int endY = bounds.bottom() + 1;
    int layerWidth = endX - startX;
    int layerHeight = endY - startY;

    int indexOffset = -(startX + startY * layerWidth);

    const bool isStaggered = orientation == Map::Hexagonal || orientation == Map::Staggered;

    // Create a queue to hold cells that need filling
    QQueue<QPoint> fillPositions;
    fillPositions.enqueue(fillOrigin);

    // Create an array that will store which cells have been processed
    // This is faster than checking if a given cell is in the region/list
    QVector<bool> processedCellsVec(layerWidth * layerHeight);
    bool *processedCells = processedCellsVec.data();

    // Loop through queued positions and fill them, while at the same time
    // checking adjacent positions to see if they should be added
    while (!fillPositions.isEmpty()) {
        const QPoint currentPoint = fillPositions.dequeue();
        const int startOfLine = currentPoint.y() * layerWidth;

        // Seek as far left as we can
        int left = currentPoint.x();
        while (left > startX && layer->cellAt(left - 1, currentPoint.y()) == matchCell) {
            --left;
            processedCells[indexOffset + startOfLine + left] = true;
        }

        // Seek as far right as we can
        int right = currentPoint.x();
        while (right + 1 < endX && layer->cellAt(right + 1, currentPoint.y()) == matchCell) {
            ++right;
            processedCells[indexOffset + startOfLine + right] = true;
        }

        // Add cells between left and right to the region
        fillRegion += QRegion(left, currentPoint.y(), right - left + 1, 1);

        bool leftColumnIsStaggered = false;
        bool rightColumnIsStaggered = false;

        // For hexagonal maps with a staggered Y-axis, we may need to extend the search range
        if (isStaggered) {
            if (staggerAxis == Map::StaggerY) {
                bool rowIsStaggered = ((layer->y() + currentPoint.y()) & 1) ^ staggerIndex;
                if (rowIsStaggered)
                    right = qMin(right + 1, endX - 1);
                else
                    left = qMax(left - 1, startX);
            } else {
                leftColumnIsStaggered = ((layer->x() + left) & 1) ^ staggerIndex;
                rightColumnIsStaggered = ((layer->x() + right) & 1) ^ staggerIndex;
            }
        }

        // Loop between left and right and check if cells above or below need
        // to be added to the queue.
        auto findFillPositions = [=,&fillPositions](int left, int right, int y) {
            bool adjacentCellAdded = false;

            for (int x = left; x <= right; ++x) {
                const int index = y * layerWidth + x;

                if (!processedCells[indexOffset + index] && layer->cellAt(x, y) == matchCell) {
                    // Do not add the cell to the queue if an adjacent cell was added.
                    if (!adjacentCellAdded) {
                        fillPositions.enqueue(QPoint(x, y));
                        adjacentCellAdded = true;
                    }
                } else {
                    adjacentCellAdded = false;
                }

                processedCells[indexOffset + index] = true;
            }
        };

        if (currentPoint.y() > startY) {
            int _left = left;
            int _right = right;

            if (isStaggered && staggerAxis == Map::StaggerX) {
                if (!leftColumnIsStaggered)
                    _left = qMax(left - 1, startX);
                if (!rightColumnIsStaggered)
                    _right = qMin(right + 1, endX - 1);
            }

            findFillPositions(_left, _right, currentPoint.y() - 1);
        }

        if (currentPoint.y() + 1 < endY) {
            int _left = left;
            int _right = right;

            if (isStaggered && staggerAxis == Map::StaggerX) {
                if (leftColumnIsStaggered)
                    _left = qMax(left - 1, startX);
                if (rightColumnIsStaggered)
                    _right = qMin(right + 1, endX - 1);
            }

            findFillPositions(_left, _right, currentPoint.y() + 1);
        }
    }

    return fillRegion;
}

QRegion TilePainter::computePaintableFillRegion(const QPoint &fillOrigin) const
{
    QRegion region = computeFillRegion(fillOrigin);

    const QRegion &selection = mMapDocument->selectedArea();
    if (!selection.isEmpty())
        region &= selection;

    return region;
}

QRegion TilePainter::computeFillRegion(const QPoint &fillOrigin) const
{
    Map *map = mMapDocument->map();
    QRegion region = fillRegion(mTileLayer, fillOrigin - mTileLayer->position(),
                                map->orientation(), map->staggerAxis(), map->staggerIndex());
    return region.translated(mTileLayer->position());
}

bool TilePainter::isDrawable(int x, int y) const
{
    const QRegion &selection = mMapDocument->selectedArea();
    if (!(selection.isEmpty() || selection.contains(QPoint(x, y))))
        return false;

    const int layerX = x - mTileLayer->x();
    const int layerY = y - mTileLayer->y();

    if (!mTileLayer->contains(layerX, layerY) && !mMapDocument->map()->infinite())
        return false;

    return true;
}

QRegion TilePainter::paintableRegion(const QRegion &region) const
{
    QRegion intersection = region;
    if (!mMapDocument->map()->infinite())
        intersection &= QRegion(mTileLayer->rect());

    const QRegion &selection = mMapDocument->selectedArea();
    if (!selection.isEmpty())
        intersection &= selection;

    return intersection;
}
