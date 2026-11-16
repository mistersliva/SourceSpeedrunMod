/*
 * crosshair.js — draws the HUD crosshair as SVG from the hud.ch* prefs.
 * Shared by hud.html (the real one) and index.html (settings preview).
 *
 *   Crosshair.svg({ style, color, size, gap, thickness, outline, opacity })
 */
(function (global) {
  'use strict';

  const STYLES = [
    { value: 'cross', label: 'Cross' },
    { value: 'cross-dot', label: 'Cross with dot' },
    { value: 't', label: 'T' },
    { value: 'dot', label: 'Dot' },
    { value: 'circle', label: 'Circle' },
    { value: 'circle-dot', label: 'Circle with dot' },
  ];

  function fromPrefs(P) {
    return {
      style: P.get('hud.chStyle'),
      color: P.get('hud.chColor'),
      size: Number(P.get('hud.chSize')),
      gap: Number(P.get('hud.chGap')),
      thickness: Number(P.get('hud.chThickness')),
      outline: !!P.get('hud.chOutline'),
      opacity: Number(P.get('hud.chOpacity')),
    };
  }

  // Sizes are CSS pixels at HUD scale 1; the caller scales the element.
  function svg(o) {
    const t = Math.max(1, o.thickness);
    const len = Math.max(1, o.size);
    const gap = Math.max(0, o.gap);
    const ow = o.outline ? 1 : 0;
    const R = gap + len + t + ow + 1;     // half extent
    const W = R * 2;
    const c = R;
    const shapes = [];
    const rect = (x, y, w, h) => shapes.push(['rect', { x, y, width: w, height: h }]);
    const half = t / 2;
    const style = o.style || 'cross';

    if (style === 'cross' || style === 'cross-dot' || style === 't') {
      if (style !== 't') rect(c - half, c - gap - len, t, len);   // top
      rect(c - half, c + gap, t, len);                            // bottom
      rect(c - gap - len, c - half, len, t);                      // left
      rect(c + gap, c - half, len, t);                            // right
    }
    if (style === 'dot' || style === 'cross-dot' || style === 'circle-dot') rect(c - half, c - half, t, t);
    if (style === 'circle' || style === 'circle-dot') shapes.push(['circle', { cx: c, cy: c, r: gap + len / 2, fill: 'none', 'stroke-width': t }]);

    const attrs = (a) => Object.entries(a).map(([k, v]) => `${k}="${v}"`).join(' ');
    const draw = (paint, extra) => shapes.map(([tag, a]) => {
      const isStroke = tag === 'circle';
      const p = isStroke ? { ...a, stroke: paint, 'stroke-width': a['stroke-width'] + extra * 2 } : {
        ...a, x: a.x - extra, y: a.y - extra, width: a.width + extra * 2, height: a.height + extra * 2, fill: paint,
      };
      return `<${tag} ${attrs(p)}/>`;
    }).join('');

    const color = /^#[0-9a-f]{3,8}$/i.test(o.color || '') ? o.color : '#ffffff';
    return `<svg xmlns="http://www.w3.org/2000/svg" width="${W}" height="${W}" viewBox="0 0 ${W} ${W}" style="opacity:${o.opacity ?? 1}" shape-rendering="crispEdges">`
      + (ow ? draw('rgba(0,0,0,0.75)', ow) : '') + draw(color, 0) + '</svg>';
  }

  global.Crosshair = { svg, fromPrefs, STYLES };
})(window);
