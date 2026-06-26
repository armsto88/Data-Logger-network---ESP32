# FieldMesh UI Style Guide — 2026

**Version:** 1.0  
**Product:** FieldMesh Dashboard  
**Audience:** Frontend developers, backend developers, UI designers, QA engineers, product owners, and repository agents  
**Primary users:** Ecologists, field technicians, researchers, environmental consultants, and project managers  

---

## 1. Purpose

FieldMesh is an environmental sensor monitoring platform designed for ecological fieldwork, research, deployment management, and long-term sensor monitoring.

This guide defines how the FieldMesh web application should look and behave as it evolves from a single-tenant monitoring dashboard into a multi-tenant SaaS platform.

The interface should feel:

- Modern and polished
- Scientifically credible
- Calm and ecological
- Data-rich without becoming visually crowded
- Reliable in both office and field conditions
- Accessible to researchers and non-technical users
- Consistent across desktop, tablet, and mobile

The goal is not to imitate a generic SaaS dashboard. FieldMesh should develop its own recognisable identity based on ecological monitoring, scientific clarity, and operational reliability.

---

## 2. Design Direction

### Scientific Calm

> Nature in the palette. Science in the structure. Operations in the interaction.

FieldMesh should feel ecological without looking decorative, rustic, or like a gardening website.

It should feel technical without becoming an industrial control panel.

Modern polish should come primarily from:

- Strong visual hierarchy
- Consistent spacing
- Clear typography
- Refined interaction feedback
- Responsive layouts
- Well-designed loading and error states
- Precise data visualisation
- Subtle depth and motion
- Clear status communication

Do not rely on glass effects, excessive gradients, animation, or bright colours to make the application appear modern.

---

## 3. Core Design Principles

### 3.1 Data First

Operational information must be visually stronger than decorative content.

The information hierarchy on most pages should be:

1. Current condition or primary result
2. Data freshness and reliability
3. Comparison or trend
4. Controls and filters
5. Supporting metadata
6. Explanatory content

A sensor value is incomplete without its:

- Unit
- Timestamp
- Source
- Data quality
- Device or node context

### 3.2 Progressive Disclosure

Show the information required for the current decision first.

Place advanced details such as the following behind secondary tabs, drawers, or expandable sections:

- Calibration data
- Firmware information
- Protocol values
- Raw payloads
- Device diagnostics
- Historical configuration
- API metadata

Do not hide important health, warning, or freshness information merely to make the interface look cleaner.

### 3.3 Consistent Semantics

The same visual treatment must always communicate the same meaning.

| Colour family | Meaning |
|---|---|
| Green | Working, connected, healthy, positive |
| Amber | Delayed, degraded, or requiring attention |
| Red | Failed, offline, destructive, or critical |
| Blue | Informational, selected, or active |
| Grey | Neutral, unavailable, unknown, or disabled |

Do not use green simply because FieldMesh is an ecological product.

### 3.4 Field Resilience

The interface must remain understandable when:

- The network connection is slow
- Some devices have not reported
- Sensor data is incomplete
- Data is stale
- A chart contains gaps
- A project has hundreds of nodes
- The user is outdoors in bright light
- The user is working on a tablet or phone
- A request fails halfway through
- Realtime updates are temporarily unavailable

### 3.5 Accessibility by Default

FieldMesh should target **WCAG 2.2 AA**.

Accessibility must be included in the component system rather than added after pages are complete.

---

## 4. Design Tokens

FieldMesh components must use semantic design tokens rather than direct colour names.

Preferred:

```tsx
<div className="bg-surface text-text-primary border-border">
```

Avoid:

```tsx
<div className="bg-green-50 text-slate-900 border-gray-200">
```

Semantic tokens allow the application to support:

- Light and dark themes
- Consistent status behaviour
- Future brand changes
- High-contrast adjustments
- Reusable component styling

---

## 5. Colour System

### 5.1 Light Theme

| Token | Value | Intended use |
|---|---:|---|
| `background` | `#F4F7F2` | Main application background |
| `surface` | `#FFFFFF` | Cards, tables, and panels |
| `surface-subtle` | `#EAF0E8` | Filter bars and grouped areas |
| `surface-raised` | `#FFFFFF` | Dialogs, popovers, and menus |
| `text-primary` | `#172019` | Main text |
| `text-secondary` | `#59645C` | Supporting text |
| `text-muted` | `#758079` | Low-priority labels |
| `border` | `#CDD7CF` | Dividers and card boundaries |
| `border-strong` | `#AEBBB1` | Stronger control boundaries |
| `primary` | `#176B3A` | Primary actions and selection |
| `primary-hover` | `#11562E` | Hover and pressed state |
| `primary-soft` | `#DCEFE2` | Selected rows and soft highlights |
| `info` | `#1D5FA7` | Information and links |
| `warning` | `#9A6700` | Delayed or degraded state |
| `danger` | `#B42318` | Failure and destructive actions |
| `unknown` | `#68736B` | Unknown or unavailable state |

### 5.2 Dark Theme

| Token | Value | Intended use |
|---|---:|---|
| `background` | `#0E1511` | Main application background |
| `surface` | `#151F19` | Cards and panels |
| `surface-subtle` | `#1A261E` | Grouped areas |
| `surface-raised` | `#1D2A22` | Popovers and dialogs |
| `text-primary` | `#F3F7F3` | Main text |
| `text-secondary` | `#A8B5AA` | Supporting text |
| `text-muted` | `#87958A` | Low-priority labels |
| `border` | `#314238` | Dividers and boundaries |
| `border-strong` | `#44594B` | Stronger boundaries |
| `primary` | `#75D092` | Primary actions |
| `primary-hover` | `#8FDEA7` | Hover and pressed state |
| `primary-soft` | `#183B26` | Selected and highlighted areas |
| `info` | `#72A7E3` | Information |
| `warning` | `#E1AD4C` | Warning |
| `danger` | `#F07B72` | Error and destructive action |
| `unknown` | `#9AA59D` | Unknown or unavailable state |

Use an off-black dark background rather than pure black.

Provide a persistent theme selector with:

- Light
- System
- Dark

### 5.3 Contrast Requirements

- Normal text: minimum contrast ratio of **4.5:1**
- Large text: minimum contrast ratio of **3:1**
- Essential icons: minimum contrast ratio of **3:1**
- Control boundaries: minimum contrast ratio of **3:1**
- Meaningful chart graphics: minimum contrast ratio of **3:1** where applicable

Do not communicate status through colour alone.

Add at least one additional cue:

- Text label
- Icon
- Shape
- Pattern
- Dash style
- Position
- Accessible description

---

## 6. Typography

Use a single primary font family for operational screens.

```css
font-family:
  InterVariable,
  Inter,
  ui-sans-serif,
  system-ui,
  -apple-system,
  BlinkMacSystemFont,
  "Segoe UI",
  sans-serif;
```

Use the system monospace stack for:

- MAC addresses
- Device IDs
- API values
- Coordinates
- Protocol fields
- Hex values
- Raw payload fragments
- Firmware versions

### 6.1 Type Scale

| Role | Size / line height | Weight |
|---|---:|---:|
| Page title | `28px / 36px` | 700 |
| Section title | `22px / 30px` | 650–700 |
| Card title | `16px / 24px` | 600 |
| Body | `16px / 24px` | 400 |
| Dense UI | `14px / 20px` | 400–500 |
| Caption | `12px / 18px` | 500 |
| KPI value | `28–36px / 1.1` | 650–700 |
| Button | `14px / 20px` | 600 |

Use tabular numerals for changing values:

```css
font-variant-numeric: tabular-nums;
```

Apply this to:

- KPI values
- Timestamps
- Sensor readings
- Tables
- Coordinates
- Battery percentages
- Counts

### 6.2 Scientific Names

Common names should use normal body text.

Scientific names should be italicised.

Example:

```text
Grasshopper warbler
Locustella naevia
```

---

## 7. Spacing System

Use a 4-pixel base spacing scale.

```text
4, 8, 12, 16, 20, 24, 32, 40, 48, 64
```

Recommended defaults:

| Context | Spacing |
|---|---:|
| Card padding | `20–24px` |
| Dense table cell vertical padding | `10–12px` |
| Form field gap | `16px` |
| Related control gap | `8–12px` |
| Section gap | `32px` |
| Page gutter on mobile | `16px` |
| Page gutter on tablet | `24px` |
| Page gutter on desktop | `32px` |

Avoid arbitrary values unless a component genuinely requires them.

---

## 8. Shape and Elevation

### 8.1 Corner Radius

| Component | Radius |
|---|---:|
| Controls | `8px` |
| Small panels | `10px` |
| Cards | `12px` |
| Large panels | `16px` |
| Modal and drawer | `16px` |
| Status pill | `999px` |

Do not make every component pill-shaped.

### 8.2 Elevation Levels

| Level | Use |
|---|---|
| Level 0 | Page background |
| Level 1 | Cards and panels |
| Level 2 | Menus, dropdowns, and popovers |
| Level 3 | Modals and blocking dialogs |

Most dashboard cards should use a border and a very soft shadow.

```css
box-shadow:
  0 1px 2px rgb(20 35 25 / 0.04),
  0 6px 18px rgb(20 35 25 / 0.05);
```

Do not use strong shadows on every element.

---

## 9. Application Layout

### 9.1 Desktop

- Persistent left navigation: `240–256px`
- Collapsed navigation rail: `68–72px`
- Top application bar: approximately `64px`
- Maximum operational content width: approximately `1600px`
- Use a 12-column layout for major page structures
- Allow charts and tables to use the full available width

### 9.2 Tablet

- Collapsible navigation rail
- Two-column dashboard layout where possible
- Filter controls may wrap onto a second row
- Charts should normally remain full width
- Interaction targets should remain at least `44–48px`

### 9.3 Mobile

Recommended primary bottom navigation:

1. Dashboard
2. Charts
3. Nodes
4. Config
5. More

Place the following under **More**:

- About
- Project management
- Account
- Theme settings
- Help
- Administrative pages

Do not shrink desktop tables until their content becomes unreadable.

Use either:

- Controlled horizontal scrolling with a sticky first column
- A deliberately designed mobile record layout
- Expandable rows for secondary fields

### 9.4 Responsive Components

Use:

- Viewport media queries for the application shell
- Container queries for reusable cards, chart headers, filter bars, and panels

Components should respond to the width available to them rather than relying entirely on the browser viewport.

---

## 10. Navigation

The main application shell should contain:

- FieldMesh logo and product name
- Active project selector
- Main navigation
- Global system or device health indicator
- User account menu
- Theme selector
- Help entry
- Connection state where relevant

### 10.1 Active Navigation State

The active item must use at least three visual cues:

- Background or side indicator
- Icon treatment
- Increased text weight

Do not use colour alone.

### 10.2 Project Switching

Project selection must be visually separate from page navigation.

The active project name must always remain visible.

When changing projects:

1. Immediately clear or replace project-scoped content
2. Display a loading state for the new project
3. Never display the previous project’s data under the new project name
4. Ensure every project-scoped query key includes `projectId`
5. Ensure every node-scoped query key includes both `projectId` and `nodeId`

---

## 11. Page Headers

Recommended structure:

```text
Dashboard
Project health and latest environmental readings

[Date range] [Refresh] [Last updated 2 min ago] [Live]
```

Page headers should normally include:

- Page title
- One-line description
- Primary page controls
- Data freshness
- Current scope
- Relevant status

Do not hide the last-updated time.

---

## 12. Dashboard

### 12.1 Information Hierarchy

Recommended dashboard order:

1. Fleet or project health
2. Current environmental overview
3. Recent trends
4. Deployment map or spatial context
5. Devices requiring attention
6. Alerts
7. Recent activity

The most important information should occupy the most visual space.

### 12.2 KPI Cards

Each KPI card should contain:

1. Short label
2. Main value
3. Unit
4. Comparison or trend
5. Data freshness
6. Optional sparkline
7. Optional warning or quality indicator

Example:

```text
AIR TEMPERATURE

18.4 °C
Down 1.2 °C over 24 hours

Last reading 4 minutes ago
```

Do not treat an upward trend as automatically positive.

Trend meaning depends on the metric.

Use neutral language unless a scientifically defined desirable direction exists.

### 12.3 Card Grouping

Do not turn every piece of information into a separate card.

Group closely related information when it supports comparison.

Examples:

- Temperature and humidity
- Soil moisture and soil temperature
- Node status and last report
- Storage usage and upload backlog

---

## 13. Status System

Use explicit operational states.

| State | Visual treatment |
|---|---|
| Live | Green dot and `Live` |
| Delayed | Amber clock and `Delayed` |
| Offline | Red disconnected icon and `Offline` |
| Never reported | Grey empty-circle icon and `Never reported` |
| Maintenance | Blue tool icon and `Maintenance` |
| Unknown | Grey question icon and `Unknown` |

Never display `Poor` merely because the current status is missing.

### 13.1 Dynamic Thresholds

Delayed and offline thresholds should be derived from the configured reporting interval.

Example:

```text
Expected every 30 minutes.
Last report received 1 hour 42 minutes ago.
```

### 13.2 Status Animation

Use pulsing animation only for genuinely active processes, such as:

- Upload in progress
- Live connection attempt
- Provisioning in progress

Do not make every live indicator pulse continuously.

---

## 14. Charts and Scientific Visualisation

Every chart should provide:

- Descriptive title
- Metric name
- Unit
- Date range
- Timezone
- Clearly labelled axes
- Visible missing-data gaps
- Data source or quality context
- Accessible textual summary or data table
- Export action where useful

### 14.1 Chart Styling

| Element | Guideline |
|---|---|
| Standard line | `2px` |
| Selected line | `3px` |
| Grid lines | Subtle and low contrast |
| Axis labels | `12–13px` |
| Tooltip | Exact timestamp, value, unit, and node |
| Crosshair | Subtle dashed line |
| Point markers | Hidden normally; visible on hover or focus |
| Legend | Interactive when series can be toggled |

Do not smooth or interpolate across missing sensor readings.

A break in a line should represent a real break in observation.

### 14.2 Multi-Series Palette

Use a dedicated chart palette instead of semantic status colours.

```text
Blue       #2F6BC1
Orange     #B77900
Green      #2E8B57
Purple     #6D4CC4
Red        #C4473A
Cyan       #1688A8
Olive      #688C2E
Magenta    #8F2D56
```

For spectral channels, use wavelength-aware colours where useful, but also provide:

- Direct labels
- Dash patterns
- Selectable emphasis
- Accessible legend controls

Do not rely on eight colours alone to distinguish eight spectral lines.

### 14.3 Recommended Chart Types

| User question | Preferred chart |
|---|---|
| How did a metric change over time? | Line chart |
| How do several nodes compare? | Multi-line chart with selection |
| What is the hourly or daily pattern? | Heat map or grouped line chart |
| How are values distributed? | Histogram or box plot |
| Are two variables related? | Scatter plot |
| How much bounded capacity remains? | Progress bar |
| What are the wind directions? | Wind rose or polar chart |
| Where are the devices? | Map plus accessible list |

Avoid pie and donut charts when users need precise comparison.

Use gauges only for genuinely bounded metrics such as:

- Battery percentage
- Storage capacity
- Upload completion

Do not use gauges for:

- Temperature
- Detection count
- Species richness
- Arbitrary sensor quality
- Unbounded measurements

---

## 15. Maps

The map must never be the only way to find or select a node.

Always provide an accompanying list or table containing:

- Node name
- Status
- Last report
- Sensor type
- Location
- Battery
- Alerts

### 15.1 Map Behaviour

- Cluster dense markers
- Preserve the viewport while filters change
- Synchronise map and list selection
- Use marker shape or icon as well as colour
- Provide large zoom controls
- Provide `Fit to project`
- Provide copy-coordinate action
- Display coordinate system and precision where relevant

### 15.2 Marker Popover

```text
North Meadow 004
Live · reported 3 minutes ago

Battery        82%
Temperature    17.8 °C
Soil moisture  34%

[Open node]
```

---

## 16. Tables and Node Lists

Tables are appropriate for ecological and technical comparison.

Do not replace useful desktop tables with oversized cards purely to appear modern.

Provide:

- Sticky header
- Sort indicators
- Search
- Filters
- Active-filter summary
- Reset-filters action
- Column visibility controls
- Pagination or virtualisation
- Comfortable and compact density modes
- Keyboard focus
- Stable column widths

Recommended columns:

```text
Node | Status | Last report | Battery | Location | Sensors | Alerts
```

On mobile, keep these fields visible:

- Node
- Status
- Last report

Move secondary information into an expandable row.

Use skeletons that closely match the final row dimensions.

---

## 17. Forms and Configuration

Labels should appear above controls.

Placeholder text must not replace a label.

Recommended field structure:

```text
Label
Optional supporting text
Input
Validation message
```

Error messages should explain:

- What happened
- Which value is invalid
- How to fix it

Avoid:

```text
Invalid value
```

Prefer:

```text
Wake interval must be between 1 and 1,440 minutes.
```

### 17.1 Configuration Changes

For configuration changes:

- Show current value
- Show proposed value
- Show affected devices
- Explain when the change becomes active
- Preserve unsaved values after recoverable errors
- Warn before leaving with unsaved changes
- Confirm destructive or fleet-wide changes

Use radio buttons for a short visible set of mutually exclusive options.

Use selects for longer option lists.

Prefer vertical radio groups.

---

## 18. Buttons and Controls

### 18.1 Action Hierarchy

Use no more than one visually dominant primary action in a local action area.

```text
Primary:      Save configuration
Secondary:    Cancel
Tertiary:     View details
Destructive:  Delete project
```

### 18.2 Button Sizes

| Context | Size |
|---|---:|
| Compact desktop visible height | `36px` |
| Compact desktop hit area | `44px` |
| Standard button | `40–44px` |
| Touch and mobile | `48px` |

Adopt an internal target size of at least `44 × 44px` where practical.

### 18.3 Icon-Only Buttons

Every icon-only button requires:

- Accessible name
- Tooltip
- Visible focus state
- Minimum hit area
- Familiar icon
- Clear disabled state

Use Lucide icons consistently at:

```text
16px, 18px, 20px, or 24px
```

Do not mix unrelated icon families.

---

## 19. Modals, Drawers, and Popovers

Use:

| Component | Use |
|---|---|
| Modal | Focused decision or destructive confirmation |
| Drawer | Inspecting or editing a node while preserving page context |
| Popover | Brief contextual information |
| Full page | Complex workflows such as project creation or provisioning |

Do not place a modal over another modal.

When a modal opens:

1. Move focus into the modal
2. Keep keyboard focus inside the modal
3. Support Escape unless closing would be unsafe
4. Return focus to the triggering control after closing

---

## 20. Loading, Empty, Stale, and Error States

Every data-driven component must support:

1. Initial loading
2. Background refresh
3. Empty result
4. No matching filter result
5. Stale data
6. Offline state
7. Network failure
8. Permission failure
9. Partial data
10. Successful content

Do not replace loaded content with a full skeleton during background refresh.

Keep existing content visible and show a subtle refresh indicator.

### 20.1 Empty State

```text
No nodes have reported yet

Register a mothership or upload the first FieldMesh data package
to begin monitoring this project.

[Register mothership]
```

### 20.2 Filtered Empty State

```text
No nodes match these filters

[Clear filters]
```

### 20.3 Error Clarity

Differentiate between:

- No data exists
- Data has not arrived yet
- The user does not have permission
- The request failed
- The device is offline
- The data is stale
- Only part of the data loaded

---

## 21. Motion and Interaction

Recommended durations:

| Interaction | Duration |
|---|---:|
| Hover and focus feedback | `100–140ms` |
| Buttons and toggles | `140–180ms` |
| Panel transitions | `180–240ms` |
| Modal and drawer | `220–280ms` |

Use ease-out when content enters.

Use ease-in when content leaves.

### 21.1 Appropriate Motion

Motion may:

- Explain spatial change
- Confirm an action
- Direct attention
- Preserve orientation
- Show progress

Motion must not:

- Delay access to data
- Continually animate live values
- Bounce panels
- Animate every chart after each refresh
- Add large parallax effects
- Distract from operational status

Support:

```css
@media (prefers-reduced-motion: reduce) {
  /* remove or simplify non-essential motion */
}
```

Use translucent or glass-like surfaces only sparingly, such as:

- Top application bar
- Map controls
- About-page storytelling

Do not use transparency behind dense tables or charts.

---

## 22. About Page

The About page may be more visually expressive than the operational pages.

It may use:

- Larger typography
- Ecological illustrations
- Soft gradients
- Animated data-flow diagrams
- Layered hardware illustrations
- Scroll-based storytelling
- PCB and protocol visualisation

Dashboard, Charts, Nodes, and Config should remain calmer and more utilitarian.

Heavy visualisations should be lazy-loaded.

Reduced-motion preferences must still be respected.

---

## 23. Performance Standards

Preserve the existing small frontend bundle as a product strength.

Recommended budgets:

| Metric | Target |
|---|---:|
| Initial JavaScript | `≤ 120 KB gzipped` |
| Individual lazy route | Preferably `≤ 80 KB gzipped` |
| Initial CSS | `≤ 35 KB gzipped` |
| Largest Contentful Paint | `≤ 2.5 seconds` |
| Interaction to Next Paint | `≤ 200ms` |
| Cumulative Layout Shift | `≤ 0.1` |

Continue to:

- Lazy-load chart-heavy routes
- Lazy-load About-page diagrams
- Reserve dimensions for charts and images
- Defer non-critical code
- Virtualise genuinely large tables
- Pause unnecessary polling while the page is hidden
- Refresh stale data when the page becomes visible
- Avoid unnecessary rerenders
- Keep realtime events compact
- Query historical data rather than streaming all raw history

---

## 24. Accessibility Definition of Done

Every UI change should verify:

- Keyboard access works
- Focus is visible
- Focus is not hidden behind sticky navigation
- Interactive targets are at least `44 × 44px` where practical
- Normal text contrast reaches `4.5:1`
- Essential UI graphics reach `3:1`
- Information is not communicated through colour alone
- Zoom to 200% does not remove functionality
- Mobile reflow does not require full-page two-dimensional scrolling
- Controls have accessible names
- Dialog focus is managed
- Charts have a textual alternative
- Tables use semantic headers
- Errors are associated with their fields
- Reduced-motion mode works
- Light and dark themes are tested
- Loading, empty, stale, partial, offline, and error states are present

Use established WAI-ARIA interaction patterns for:

- Tabs
- Dialogs
- Comboboxes
- Menus
- Tooltips
- Listboxes
- Trees
- Disclosure controls

Do not invent custom keyboard behaviour unnecessarily.

---

## 25. Tailwind Token Foundation

Example CSS variables:

```css
:root {
  --fm-background: 244 247 242;
  --fm-surface: 255 255 255;
  --fm-surface-subtle: 234 240 232;
  --fm-surface-raised: 255 255 255;

  --fm-text-primary: 23 32 25;
  --fm-text-secondary: 89 100 92;
  --fm-text-muted: 117 128 121;

  --fm-border: 205 215 207;
  --fm-border-strong: 174 187 177;

  --fm-primary: 23 107 58;
  --fm-primary-hover: 17 86 46;
  --fm-primary-soft: 220 239 226;

  --fm-info: 29 95 167;
  --fm-warning: 154 103 0;
  --fm-danger: 180 35 24;
  --fm-unknown: 104 115 107;
}

.dark {
  --fm-background: 14 21 17;
  --fm-surface: 21 31 25;
  --fm-surface-subtle: 26 38 30;
  --fm-surface-raised: 29 42 34;

  --fm-text-primary: 243 247 243;
  --fm-text-secondary: 168 181 170;
  --fm-text-muted: 135 149 138;

  --fm-border: 49 66 56;
  --fm-border-strong: 68 89 75;

  --fm-primary: 117 208 146;
  --fm-primary-hover: 143 222 167;
  --fm-primary-soft: 24 59 38;

  --fm-info: 114 167 227;
  --fm-warning: 225 173 76;
  --fm-danger: 240 123 114;
  --fm-unknown: 154 165 157;
}
```

Example Tailwind mapping:

```ts
export default {
  theme: {
    extend: {
      colors: {
        background: "rgb(var(--fm-background) / <alpha-value>)",
        surface: "rgb(var(--fm-surface) / <alpha-value>)",
        "surface-subtle": "rgb(var(--fm-surface-subtle) / <alpha-value>)",
        "surface-raised": "rgb(var(--fm-surface-raised) / <alpha-value>)",

        "text-primary": "rgb(var(--fm-text-primary) / <alpha-value>)",
        "text-secondary": "rgb(var(--fm-text-secondary) / <alpha-value>)",
        "text-muted": "rgb(var(--fm-text-muted) / <alpha-value>)",

        border: "rgb(var(--fm-border) / <alpha-value>)",
        "border-strong": "rgb(var(--fm-border-strong) / <alpha-value>)",

        primary: "rgb(var(--fm-primary) / <alpha-value>)",
        "primary-hover": "rgb(var(--fm-primary-hover) / <alpha-value>)",
        "primary-soft": "rgb(var(--fm-primary-soft) / <alpha-value>)",

        info: "rgb(var(--fm-info) / <alpha-value>)",
        warning: "rgb(var(--fm-warning) / <alpha-value>)",
        danger: "rgb(var(--fm-danger) / <alpha-value>)",
        unknown: "rgb(var(--fm-unknown) / <alpha-value>)",
      },
    },
  },
};
```

---

## 26. Component Requirements

### 26.1 Card

A standard card should support:

- Title
- Description
- Optional status
- Optional action area
- Content
- Loading state
- Error state
- Empty state
- Dark mode
- Responsive padding

### 26.2 Status Badge

A status badge should support:

- Icon
- Text label
- Semantic colour
- Accessible text
- Compact and standard sizes
- Tooltip where thresholds require explanation

### 26.3 Metric Card

A metric card should support:

- Label
- Value
- Unit
- Trend
- Comparison period
- Freshness
- Quality state
- Optional sparkline
- Optional click-through action

### 26.4 Data Table

A data table should support:

- Sorting
- Filtering
- Search
- Pagination or virtualisation
- Column selection
- Loading state
- Empty state
- Error state
- Keyboard navigation
- Accessible headers
- Responsive behaviour

### 26.5 Chart Panel

A chart panel should support:

- Title
- Description
- Unit
- Timezone
- Date range
- Series selector
- Legend
- Loading state
- Empty state
- Error state
- Accessible summary
- Export action
- Responsive toolbar

### 26.6 Form Field

A form field should support:

- Label
- Description
- Required state
- Input
- Validation
- Error message
- Disabled state
- Read-only state
- Help action

---

## 27. FieldMesh-Specific UX Rules

### 27.1 Data Freshness

Every current value should make it possible to determine:

- When it was recorded
- When it was received
- Which node produced it
- Whether it is stale
- Whether it passed validation

### 27.2 Device Health

Device health must distinguish:

- Connection state
- Battery state
- Sensor state
- Storage state
- Upload backlog
- Configuration state
- Firmware state

Do not combine all failures into one vague `Poor` status.

### 27.3 Missing Data

Do not treat missing data as zero.

Use:

- Em dash
- `No reading`
- `Not reported`
- `Unavailable`

depending on context.

### 27.4 Time and Timezones

Always display the active timezone in chart and export contexts.

For ambiguous timestamps, provide:

- Local project time
- UTC where technically relevant
- Full timestamp in tooltip or details

### 27.5 Units

Units must be consistent and visible.

Examples:

- `°C`
- `% RH`
- `% VWC`
- `m/s`
- `mm`
- `nm`
- `V`
- `dB`
- `µmol/m²/s`

Do not mix units within a chart without clear axis separation.

### 27.6 Coordinates

Coordinates should:

- Use consistent decimal precision
- Offer copy action
- Identify coordinate reference system where relevant
- Avoid implying false precision

---

## 28. Realtime Behaviour

Realtime updates should not create visual instability.

When new values arrive:

- Update the relevant value
- Preserve user selections
- Preserve scroll position
- Avoid rerendering the full page
- Avoid replaying full chart animations
- Avoid layout shift
- Show a subtle update acknowledgement where useful

Historical chart data should normally remain query-based.

Realtime should focus on:

- Latest device snapshot
- Node status
- Alerts
- Upload completion
- New activity
- Configuration acknowledgement

---

## 29. Content Style

FieldMesh text should be:

- Direct
- Calm
- Specific
- Non-alarmist
- Technically accurate
- Understandable to non-developers

Prefer:

```text
Node has not reported for 2 hours.
```

Avoid:

```text
Critical communication failure detected!
```

Prefer:

```text
Upload failed. Check the device connection and try again.
```

Avoid:

```text
Something went wrong.
```

Buttons should describe actions:

- Save configuration
- Register mothership
- Open node
- Retry upload
- Clear filters
- Export CSV

Avoid vague labels such as:

- Continue
- Submit
- Click here
- Do it
- OK

---

## 30. Things FieldMesh Should Avoid

- Green text and green charts everywhere
- Excessive glassmorphism
- Low-contrast grey text
- Tiny icon-only controls
- Pill-shaped styling on every control
- More than one dominant action per panel
- Gauges without meaningful bounds
- Charts without units
- Smoothed lines across missing data
- Status represented only by coloured dots
- Replacing useful tables with decorative cards
- Animating charts after every refresh
- Full-page spinners during background updates
- Hidden timestamps
- Showing `Poor` when the state is unknown
- Nested modals
- Destructive actions beside primary actions
- Layout shifts when live data arrives
- Permanent credentials inside QR codes
- Dense animation on operational screens
- Using placeholder text instead of labels
- Treating missing readings as zero
- Displaying old project data after project switching
- Using colour alone for spectral series
- Mixing multiple icon families
- Direct backend calls from arbitrary components
- One-off colours and spacing values inside feature code

---

## 31. Recommended Review Checklist

Before merging a new page or major component, verify:

### Visual

- [ ] Uses semantic tokens
- [ ] Matches FieldMesh typography
- [ ] Uses the spacing scale
- [ ] Uses consistent radii and elevation
- [ ] Works in light and dark themes
- [ ] Maintains clear hierarchy

### Responsive

- [ ] Works on desktop
- [ ] Works on tablet
- [ ] Works on mobile
- [ ] Does not create accidental page-level horizontal scrolling
- [ ] Keeps primary actions available

### Data

- [ ] Shows units
- [ ] Shows freshness
- [ ] Distinguishes stale, missing, and failed data
- [ ] Preserves gaps in time-series data
- [ ] Does not treat missing values as zero

### Interaction

- [ ] Keyboard accessible
- [ ] Focus state visible
- [ ] Touch targets large enough
- [ ] Loading state present
- [ ] Empty state present
- [ ] Error state present
- [ ] Offline or stale state considered
- [ ] Background refresh does not remove existing content

### Security and Project Scope

- [ ] Active project is visible
- [ ] Project-scoped queries include `projectId`
- [ ] Node-scoped queries include `projectId` and `nodeId`
- [ ] Previous-project data cannot flash after switching
- [ ] Permission failures are handled explicitly

### Performance

- [ ] Heavy code is lazy-loaded
- [ ] Charts do not rerender unnecessarily
- [ ] Layout dimensions are reserved
- [ ] No unnecessary dependency added
- [ ] Large tables use pagination or virtualisation where needed

---

## 32. Reference Standards

This guide is based on principles from the following standards and design systems:

- WCAG 2.2
- WAI-ARIA Authoring Practices Guide
- Apple Human Interface Guidelines
- Material Design 3
- Microsoft Fluent 2
- IBM Carbon Design System
- GOV.UK Design System
- MDN Web Docs
- web.dev Core Web Vitals
- Supabase frontend and realtime architecture guidance

Useful references:

- <https://www.w3.org/TR/WCAG22/>
- <https://www.w3.org/WAI/ARIA/apg/patterns/>
- <https://developer.apple.com/design/human-interface-guidelines/>
- <https://m3.material.io/>
- <https://fluent2.microsoft.design/>
- <https://carbondesignsystem.com/>
- <https://design-system.service.gov.uk/>
- <https://developer.mozilla.org/>
- <https://web.dev/articles/vitals>
- <https://supabase.com/docs/guides/realtime>

---

## 33. Summary

FieldMesh should use a **scientifically calm** design language.

The platform should combine:

- Ecological warmth
- Scientific precision
- Modern SaaS polish
- Operational reliability
- Accessible interaction
- Strong data visualisation
- Responsive field usability

The interface should not attempt to look modern through decoration alone.

It should feel modern because it is:

- Clear
- Fast
- Consistent
- Responsive
- Accessible
- Trustworthy
- Thoughtfully detailed