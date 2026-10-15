#ifndef INDEX_H
#define INDEX_H

const char MAIN_PAGE[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>Darren's Disco</title>
    <style>
        :root {
            --bg-color: #121212;
            --surface-color: #1e1e1e;
            --primary-color: #bb86fc;
            --secondary-color: #03dac6;
            --error-color: #cf6679;
            --text-color: #e0e0e0;
            --text-secondary: #a0a0a0;
            --border-color: #333333;
        }
        body {
            font-family: 'Segoe UI', Tahoma, Geneva, Verdana, sans-serif;
            background-color: var(--bg-color);
            color: var(--text-color);
            margin: 0;
            padding: 10px;
            display: flex;
            flex-direction: column;
            align-items: center;
        }
        h1 { font-size: 1.5rem; margin-bottom: 10px; text-align: center; color: var(--primary-color); }
        /* (The circular-swatch CSS I added here was reverted: it changed the size and
           shape of the Colour page boxes, which was not wanted. The white ring for
           dark colours belongs on the NEW swatch row in Stage 2, not on these. */

        .nav-tabs {
            display: flex;
            gap: 5px;
            margin-bottom: 15px;
            width: 100%;
            max-width: 600px;
            overflow-x: auto;
            padding-bottom: 5px;
        }
        .tab-btn {
            background-color: var(--surface-color);
            color: var(--text-secondary);
            border: 1px solid var(--border-color);
            padding: 10px 15px;
            cursor: pointer;
            border-radius: 5px;
            font-weight: bold;
            flex-shrink: 0;
            transition: 0.2s;
        }
        .tab-btn.active {
            background-color: var(--primary-color);
            color: #000;
            border-color: var(--primary-color);
        }
        .tab-btn:disabled {
            opacity: 0.3;
            cursor: not-allowed;
        }
        .card {
            background-color: var(--surface-color);
            border: 1px solid var(--border-color);
            border-radius: 8px;
            padding: 15px;
            width: 100%;
            max-width: 600px;
            box-sizing: border-box;
            margin-bottom: 15px;
            display: none;
        }
        .card.active { display: block; }
        .control-group {
            margin-bottom: 12px;
            display: flex;
            flex-direction: column;
        }
        label { font-size: 0.9rem; margin-bottom: 5px; color: var(--text-secondary); }
        select, input[type="text"], input[type="number"], input[type="range"] {
            background-color: #2c2c2c;
            color: var(--text-color);
            border: 1px solid var(--border-color);
            padding: 8px;
            border-radius: 4px;
            font-size: 1rem;
            box-sizing: border-box;
        }
        select:disabled, input:disabled {
            opacity: 0.4;
            cursor: not-allowed;
        }
        /* ---------------------------------------------------------------------
           Range sliders: large, easy to grab, and a drag NEVER scrolls the page.
           The generic rule above left the hit area ~8px high with the browser's
           default thin thumb, so a slightly imprecise press landed on the page
           background and scrolled it instead of moving the slider.
           - height 44px  : a big touch target, hard to miss
           - touch-action : none  -> a drag is claimed by the slider, not the page
           - a 30px round thumb with the track drawn behind it
           --------------------------------------------------------------------- */
        input[type="range"] {
            -webkit-appearance: none;
            appearance: none;
            width: 100%;
            height: 44px;
            padding: 0;
            margin: 0;
            background: transparent;
            border: none;
            touch-action: none;
            cursor: pointer;
        }
        input[type="range"]::-webkit-slider-runnable-track {
            height: 8px;
            border-radius: 4px;
            background: #2c2c2c;
            border: 1px solid var(--border-color);
        }
        input[type="range"]::-webkit-slider-thumb {
            -webkit-appearance: none;
            width: 30px;
            height: 30px;
            margin-top: -12px;          /* centre the thumb on the track */
            border-radius: 50%;
            background: var(--primary-color);
            border: 2px solid #000;
        }
        input[type="range"]:active::-webkit-slider-thumb {
            background: #ffffff;
            transform: scale(1.1);
        }
        input[type="range"]::-moz-range-track {
            height: 8px;
            border-radius: 4px;
            background: #2c2c2c;
        }
        input[type="range"]::-moz-range-thumb {
            width: 30px;
            height: 30px;
            border-radius: 50%;
            background: var(--primary-color);
            border: 2px solid #000;
        }
        input[type="range"]:focus { outline: none; }
        .btn {
            background-color: var(--secondary-color);
            color: #000;
            border: none;
            padding: 12px;
            font-weight: bold;
            border-radius: 5px;
            cursor: pointer;
            width: 100%;
            font-size: 1rem;
            margin-top: 5px;
        }
        .btn-secondary {
            background-color: var(--surface-color);
            color: var(--text-color);
            border: 1px solid var(--border-color);
        }

        /* Pinned Live Preview Container */
        .sticky-preview-container {
            position: sticky;
            top: 0;
            z-index: 100;
            background: var(--surface-color);
            border: 1px solid var(--secondary-color);
            border-radius: 6px;
            padding: 10px;
            margin-bottom: 15px;
            box-shadow: 0 4px 12px rgba(0,0,0,0.5);
        }
        .live-preview-canvas {
            width: 100%;
            height: 220px;
            background-color: #000;
            border-radius: 4px;
            border: 1px solid var(--border-color);
            display: block;
        }

        #calTestBtn {
            width: 100%;
            height: 70px;
            font-size: 1.2rem;
            font-weight: bold;
            border-radius: 8px;
            border: 2px solid #fff;
            cursor: pointer;
            display: flex;
            align-items: center;
            justify-content: center;
            gap: 15px;
            background: #222;
            color: #fff;
            margin-top: 10px;
        }
        .color-block {
            display: inline-block;
            width: 25px;
            height: 25px;
            border-radius: 4px;
            border: 1px solid #fff;
        }

        /* Palette Builder Styling */
        .theme-group, .count-group {
            display: flex;
            gap: 5px;
            margin-bottom: 12px;
            flex-wrap: wrap;
        }
        .theme-btn, .count-btn {
            flex: 1;
            background-color: #2c2c2c;
            color: var(--text-color);
            border: 1px solid var(--border-color);
            padding: 8px;
            border-radius: 4px;
            cursor: pointer;
            text-align: center;
            font-weight: bold;
            font-size: 0.9rem;
            transition: 0.2s;
            min-width: 70px;
        }
        .theme-btn.active, .count-btn.active {
            background-color: var(--secondary-color);
            color: #000;
            border-color: var(--secondary-color);
        }
        .count-btn.hidden { display: none; }
        .color-boxes-row {
            display: flex;
            gap: 6px;
            margin-top: 8px;
            margin-bottom: 15px;
        }
        .slot-box {
            flex: 1;
            height: 45px;
            border-radius: 4px;
            border: 1px solid var(--border-color);
            background: #2c2c2c;
            display: flex;
            align-items: center;
            justify-content: center;
            overflow: hidden;
            transition: 0.2s;
        }
        .slot-box.disabled {
            opacity: 0.25;
            pointer-events: none;
            background: #151515;
        }
        .slot-box input[type="color"] {
            border: none;
            width: 100%;
            height: 100%;
            background: transparent;
            cursor: pointer;
            padding: 0;
        }
        .slot-box input[type="color"]::-webkit-color-swatch-wrapper { padding: 0; }
        .slot-box input[type="color"]::-webkit-color-swatch { border: none; border-radius: 4px; }

        /* Pattern List & Parameter Table Styles */
        .pattern-grid {
            display: grid;
            grid-template-columns: 1fr;
            gap: 8px;
            margin-top: 10px;
        }
        .pattern-item {
            background-color: #2c2c2c;
            border: 1px solid var(--border-color);
            padding: 12px 15px;
            border-radius: 5px;
            cursor: pointer;
            font-weight: bold;
            transition: 0.2s;
            text-align: left;
            color: var(--text-color);
        }
        .pattern-item:hover {
            background-color: #383838;
            border-color: var(--secondary-color);
        }
        .pattern-item.vu-disabled { opacity: 0.35; pointer-events: none; }
        .pattern-item.active {
            background-color: #283838;
            border-color: var(--secondary-color);
            color: var(--secondary-color);
            box-shadow: 0 0 8px rgba(3, 218, 198, 0.4);
        }
        
        .param-table {
            width: 100%;
            border-collapse: collapse;
            margin-top: 10px;
        }
        .param-table th, .param-table td {
            border: 1px solid var(--border-color);
            padding: 8px;
            text-align: center;
            font-size: 0.9rem;
        }
        .param-table th {
            background-color: #252525;
            color: var(--primary-color);
        }
        .param-table td.name-col {
            text-align: left;
            font-weight: bold;
            white-space: nowrap;
            width: 130px;
        }
        .param-table input[type="checkbox"] {
            appearance: none;
            -webkit-appearance: none;
            width: 22px;
            height: 22px;
            background-color: #ffffff;
            border: 2px solid var(--primary-color);
            border-radius: 4px;
            cursor: pointer;
            display: inline-flex;
            align-items: center;
            justify-content: center;
            position: relative;
            vertical-align: middle;
        }
        .param-table input[type="checkbox"]:disabled { cursor: not-allowed; }
        .param-table input[type="checkbox"]:checked::after {
            content: '✔';
            font-size: 16px;
            font-weight: bold;
            color: #000000;
        }

        .playlist-item {
            background-color: #2c2c2c;
            border: 1px solid var(--border-color);
            border-radius: 6px;
            padding: 10px 12px;
            margin-bottom: 8px;
            display: flex;
            align-items: center;
            justify-content: space-between;
            gap: 8px;
            box-sizing: border-box;
            width: 100%;
        }
        .playlist-item-index {
            font-weight: bold;
            color: var(--text-secondary);
            font-size: 0.9rem;
            min-width: 20px;
            flex-shrink: 0;
        }
        .playlist-item input[type="text"] {
            flex: 1;
            padding: 6px 8px;
            font-size: 0.95rem;
            font-weight: bold;
            color: var(--secondary-color);
            background-color: #222;
            min-width: 0;
        }
        .playlist-actions {
            display: flex;
            align-items: center;
            gap: 4px;
            flex-shrink: 0;
        }
        .playlist-btn {
            background-color: #383838;
            color: var(--text-color);
            border: 1px solid var(--border-color);
            border-radius: 4px;
            width: 30px;
            height: 30px;
            cursor: pointer;
            font-weight: bold;
            display: flex;
            align-items: center;
            justify-content: center;
            font-size: 0.9rem;
            flex-shrink: 0;
        }
        .playlist-btn:hover { background-color: var(--primary-color); color: #000; }
        .playlist-btn.edit-btn:hover { background-color: var(--secondary-color); color: #000; }
        .playlist-btn.delete-btn:hover { background-color: var(--error-color); color: #fff; }
        .playlist-btn.timeline-add-btn:hover { background-color: var(--secondary-color); color: #000; }

        /* Timeline Page Styles */
        .timeline-wrapper {
            overflow-x: auto;
            margin-top: 10px;
            border: 1px solid var(--border-color);
            border-radius: 6px;
            background: #181818;
        }
        .timeline-grid {
            display: grid;
            grid-template-columns: 50px repeat(11, minmax(65px, 1fr));
            min-width: 800px;
        }
        .timeline-header-cell, .timeline-row-label {
            background-color: #252525;
            color: var(--primary-color);
            font-size: 0.8rem;
            font-weight: bold;
            text-align: center;
            padding: 8px 4px;
            border-bottom: 1px solid var(--border-color);
            border-right: 1px solid var(--border-color);
            display: flex;
            align-items: center;
            justify-content: center;
        }
        .timeline-cell {
            background-color: #222;
            border-bottom: 1px solid var(--border-color);
            border-right: 1px solid var(--border-color);
            min-height: 50px;
            padding: 4px;
            display: flex;
            align-items: center;
            justify-content: center;
            cursor: pointer;
            transition: background 0.2s;
            position: relative;
        }
        .timeline-cell:hover { background-color: #2a2a2a; }
        .timeline-cell.highlight-valid { background-color: rgba(3, 218, 198, 0.2); border-color: var(--secondary-color); }
        .timeline-cell.highlight-invalid { background-color: rgba(207, 102, 121, 0.2); border-color: var(--error-color); }
        
        .timeline-block {
            background-color: #2c3e50;
            border: 1px solid var(--secondary-color);
            color: var(--text-color);
            border-radius: 4px;
            width: 100%;
            height: 100%;
            min-height: 42px;
            padding: 4px;
            box-sizing: border-box;
            font-size: 0.75rem;
            display: flex;
            flex-direction: column;
            justify-content: space-between;
            cursor: grab;
            position: relative;
        }
        .timeline-block:active { cursor: grabbing; }
        .timeline-block-title { font-weight: bold; white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }
        .timeline-block-delete {
            position: absolute;
            top: 2px;
            right: 2px;
            background: rgba(0,0,0,0.6);
            color: var(--error-color);
            border: none;
            border-radius: 50%;
            width: 16px;
            height: 16px;
            font-size: 10px;
            display: flex;
            align-items: center;
            justify-content: center;
            cursor: pointer;
        }

        /* Floating cursor element during placement/move */
        #floatingItem {
            position: fixed;
            pointer-events: none;
            z-index: 1000;
            background: var(--secondary-color);
            color: #000;
            padding: 6px 10px;
            border-radius: 4px;
            font-weight: bold;
            font-size: 0.85rem;
            box-shadow: 0 4px 12px rgba(0,0,0,0.6);
            display: none;
            transform: translate(-50%, -50%);
        }
    </style>
</head>
<body>

    <h1>Darren's Disco</h1>

    <div class="nav-tabs">
        <button class="tab-btn active" onclick="switchTab('master')">Master</button>
        <button class="tab-btn" onclick="switchTab('calibrate')">Colours</button>
        <button class="tab-btn" onclick="switchTab('patterns')">Patterns</button>
        <button id="tabBtn240" class="tab-btn" onclick="switchTab('240v')" disabled>240V Display</button>
        <button id="tabBtnLED" class="tab-btn" onclick="switchTab('ws2811')" disabled>LED Display</button>
        <button class="tab-btn" onclick="switchTab('playlist')">Playlist</button>
        <button class="tab-btn" onclick="switchTab('timeline')">Timeline</button>
    </div>

    <!-- MASTER TAB -->
    <div id="tab-master" class="card active">
        <h3>Master System Controls</h3>
        <div class="control-group">
            <label>Audio Detect</label>
            <select id="audioEnabled" onchange="setAudioDetect(this.value)">
                <option value="true">Enabled</option>
                <option value="false">Disabled (Clock Timer Only)</option>
            </select>
        </div>
        <div class="control-group">
            <label>Fallback Clock Speed (Seconds)</label>
            <input type="number" id="clockDelay" step="0.1" min="0.1" max="5.0">
        </div>
        <div class="control-group">
            <label>Antifreeze Timeout (Seconds)</label>
            <input type="number" id="antifreeze" min="1" max="9" onchange="setAntifreeze(this.value)">
        </div>
        <div class="control-group">
            <label>Brightness (Maximum) — <span id="brVal">100</span>%</label>
            <input type="range" id="masterBrightness" min="0" max="100" step="1" value="100"
                   oninput="document.getElementById('brVal').innerText = this.value"
                   onchange="sendBrightness(this.value)">
        </div>
        <div class="control-group">
            <div id="netStatus" style="display:none;"></div>
            <div id="diagInfo" style="display:none;"></div>
            <div id="leakInfo" style="display:none;"></div>
        </div>


        <button class="btn" onclick="saveSettings()">Save & Apply Settings</button>
    </div>

    <!-- COLOURS / CALIBRATION TAB -->
    <div id="tab-calibrate" class="card">
        <h3>Port Colour Order Calibration</h3>
        <p>Select your port to instantly illuminate the first 3 pixels. Click the test button to cycle wire orders until the physical strip matches the <b>Expected RGB Order</b>!</p>
        
        <div class="control-group">
            <label>Select Port (1 - 10)</label>
            <select id="calPort" onchange="updatePortLabels()">
                <option value="0">Port 1</option><option value="1">Port 2</option><option value="2">Port 3</option>
                <option value="3">Port 4</option><option value="4">Port 5</option><option value="5">Port 6</option>
                <option value="6">Port 7</option><option value="7">Port 8</option><option value="8">Port 9</option><option value="9">Port 10</option>
            </select>
        </div>

        <button id="calTestBtn" onclick="testPortPressed()">
            <span>TEST <span id="testPortNum">PORT 1</span></span>
            <span style="display:inline-flex; gap:5px;">
                <span class="color-block" style="background:#FF0000;" title="Red Group"></span>
                <span class="color-block" style="background:#00FF00;" title="Green Group"></span>
                <span class="color-block" style="background:#0000FF;" title="Blue Group"></span>
            </span>
        </button>

        <button class="btn" style="margin-top:15px;" onclick="saveCalibration()">SAVE <span id="savePortNum">PORT 1</span></button>

        <hr style="border: 0; border-top: 1px solid var(--border-color); margin: 25px 0 15px 0;">

        <h3>Theme & Custom Palette Builder</h3>
        
        <label>Select Theme Preset</label>
        <div class="theme-group">
            <button class="theme-btn active" onclick="themePreviewEnabled = true; setTheme('Disco')">Disco</button>
            <button class="theme-btn" onclick="themePreviewEnabled = true; setTheme('Christmas')">Christmas</button>
            <button class="theme-btn" onclick="themePreviewEnabled = true; setTheme('Halloween')">Halloween</button>
            <button class="theme-btn" onclick="themePreviewEnabled = true; setTheme('Animations')">Animations</button>
        </div>

        <!-- The "Number of Colors" selector was REMOVED. All themes now have eight
             colours, so there was nothing to select - a theme that should behave as
             four repeats its first four into slots 5-8. -->
        <label>Color Slots (Active vs Greyed Out)</label>
        <div class="color-boxes-row" id="colorBoxesContainer"></div>

        <button class="btn" style="margin-top: 10px;" onclick="saveThemePalette()">Save Theme Palette Settings</button>
    </div>

    <!-- PATTERNS SELECTION TAB -->
    <div id="tab-patterns" class="card">
        <h3>Pattern Selection</h3>
        <p>Choose a pattern to unlock configuration tabs:</p>
        <div class="pattern-grid">
            <button class="pattern-item" onclick="choosePattern('Moving Dot', this)">Moving Dot</button>
            <button class="pattern-item" onclick="choosePattern('Chase 1 - 10', this)">Chase 1 - 10</button>
            <button class="pattern-item" onclick="choosePattern('Chase 10 - 1', this)">Chase 10 - 1</button>
            <button class="pattern-item" onclick="choosePattern('Explosion', this)">Explosion</button>
            <button class="pattern-item" onclick="choosePattern('Knight Rider', this)">Knight Rider</button>
            <button data-vu="1" class="pattern-item" onclick="choosePattern('VU Meter Left', this)">VU Meter Left</button>
            <button data-vu="1" class="pattern-item" onclick="choosePattern('VU Meter Right', this)">VU Meter Right</button>
            <button data-vu="1" class="pattern-item" onclick="choosePattern('VU Meter Dual', this)">VU Meter Dual</button>
            <button data-vu="1" class="pattern-item" onclick="choosePattern('VU Meter Centre', this)">VU Meter Centre</button>
        </div>
    </div>

    <!-- 240V DISPLAY TAB -->
    <div id="tab-240v" class="card">
        <h3 id="configPatternTitle240" style="color: var(--secondary-color);">Pattern Title</h3>
        <p>Configure parameters for 240V display below:</p>

        <table class="param-table">
            <thead>
                <tr><th>Parameter</th><th>Setting</th></tr>
            </thead>
            <tbody>
                <tr>
                    <td class="name-col">Speed</td>
                    <td>
                        <input type="range" id="paramSpeed240" min="0.01" max="1.0" step="0.01" value="0.50" oninput="liveParamChanged('240V')">
                        <span id="speedVal240" style="font-size:0.85rem; display:block; color:var(--primary-color);">0.50s</span>
                    </td>
                </tr>
                <tr>
                    <td class="name-col">Quantity</td>
                    <td>
                        <input type="range" id="paramQuantity240" min="1" max="4" step="1" value="1" oninput="liveParamChanged('240V')">
                        <span id="qtyVal240" style="font-size:0.85rem; display:block; color:var(--primary-color);">1</span>
                    </td>
                </tr>
                <tr>
                    <td class="name-col">Size</td>
                    <td>
                        <input type="range" id="paramSize240" min="1" max="4" step="1" value="1" oninput="liveParamChanged('240V')">
                        <span id="sizeVal240" style="font-size:0.85rem; display:block; color:var(--primary-color);">1</span>
                    </td>
                </tr>
                <tr>
                    <td class="name-col">Wrap</td>
                    <td><input type="checkbox" id="paramWrap240" onchange="liveParamChanged('240V')"></td>
                </tr>
                <tr>
                    <td class="name-col">Invert</td>
                    <td><input type="checkbox" id="paramInvert240" onchange="liveParamChanged('240V')"></td>
                </tr>
                <tr style="background-color: #1a1a1a;">
                    <td colspan="2" style="text-align: center; font-weight: bold; color: var(--secondary-color); padding: 8px; border-top: 2px solid var(--secondary-color);">VU Meter Parameters</td>
                </tr>
                <tr>
                    <td class="name-col">Attack 240</td>
                    <td>
                        <input type="range" id="paramAttack240_240" min="0.01" max="1.0" step="0.01" value="0.10" oninput="liveParamChanged('240V')">
                        <span id="att240Val240" style="font-size:0.85rem; display:block; color:var(--primary-color);">0.10s</span>
                    </td>
                </tr>
                <tr>
                    <td class="name-col">Decay 240</td>
                    <td>
                        <input type="range" id="paramDecay240_240" min="0.01" max="1.0" step="0.01" value="0.30" oninput="liveParamChanged('240V')">
                        <span id="dec240Val240" style="font-size:0.85rem; display:block; color:var(--primary-color);">0.30s</span>
                    </td>
                </tr>
                <tr style="background-color: #1a1a1a;">
                    <td colspan="2" style="text-align: center; font-weight: bold; color: var(--secondary-color); padding: 8px; border-top: 2px solid var(--secondary-color);">Audio Parameters</td>
                </tr>
                <tr>
                    <td class="name-col">Music Source</td>
                    <td>
                        <select id="paramMusicSource240" style="width: 100%;" onchange="liveParamChanged('240V')">
                            <option value="BASS">Bass</option>
                            <option value="FULL RANGE">Full Range</option>
                        </select>
                    </td>
                </tr>
                <tr>
                    <td class="name-col">Frequency</td>
                    <td>
                        <input type="range" id="paramFrequency240" min="20" max="100" step="1" value="60" oninput="liveParamChanged('240V')">
                        <span id="freqVal240" style="font-size:0.85rem; display:block; color:var(--primary-color);">60Hz</span>
                    </td>
                </tr>
                <tr>
                    <td class="name-col">Beat Multiplier</td>
                    <td>
                        <input type="range" id="paramBeatMult240" min="1" max="4" step="1" value="1" oninput="liveParamChanged('240V')">
                        <span id="beatVal240" style="font-size:0.85rem; display:block; color:var(--primary-color);">1x</span>
                    </td>
                </tr>
            </tbody>
        </table>

        <button class="btn" style="margin-top: 15px;" onclick="savePatternConfig('240V')">Save Pattern Settings</button>
        <button id="playlistActionButton240" class="btn" style="margin-top: 8px; background-color: var(--primary-color); color: #000;" onclick="handlePlaylistAction('240V')">Save to Playlist</button>
    </div>

    <!-- LED DISPLAY TAB -->
    <div id="tab-ws2811" class="card">
        <div class="sticky-preview-container">
            <div class="control-group" style="margin-bottom: 5px;">
                <label style="color: var(--secondary-color); font-weight: bold;">Physical Layout Geometry</label>
                <select id="patternLayoutSelectLED" onchange="updateLivePreviewLayout('LED')">
                    <option value="STAR">STAR (10 Equal Rays)</option>
                    <option value="GRID HORIZONTAL">GRID HORIZONTAL (Stacked Rows)</option>
                    <option value="GRID VERTICAL">GRID VERTICAL (Columns)</option>
                    <option value="LINE">LINE (Dual Rows + Curved Ends)</option>
                </select>
            </div>
            <canvas id="livePreviewCanvasLED" class="live-preview-canvas"></canvas>
        </div>

        <h3 id="configPatternTitleLED" style="color: var(--secondary-color);">Pattern Title</h3>
        <p>Configure parameters for LED display below:</p>

        <table class="param-table">
            <thead>
                <tr><th>Parameter</th><th>Setting</th></tr>
            </thead>
            <tbody>
                <tr>
                    <td class="name-col">Beat Sensitivity</td>
                    <td>
                        <!-- Beat sensitivity: LEFT = fewer, only strong kicks.
                             RIGHT = more beats, catches quieter/denser music.
                             Global: one detector drives BOTH the strips and the 240V. -->
                        <input type="range" id="paramBeatSens" min="0.01" max="1.00" step="0.01" value="0.20" oninput="liveParamChanged('LED')">
                        <span id="beatSensVal" style="font-size:0.85rem; display:block; color:var(--primary-color);">0.20</span>
                    </td>
                </tr>
                <tr>
                    <td class="name-col">Colour</td>
                    <td>
                        <!-- One box for the dot/chase patterns; four for the VU meters.
                             The four share the same total width as the single box (they
                             are flex siblings, so the row length never changes). -->
                        <div id="colourBoxesLED" style="display:flex; gap:4px; width:100%;">
                            <input type="color" id="paramColourLED0" value="#ffffff" oninput="liveParamChanged('LED')" style="flex:1 1 0; min-width:0; width:100%; height:34px; padding:2px; border:1px solid var(--border-color); border-radius:4px; background:var(--surface-color); cursor:pointer;">
                            <input type="color" id="paramColourLED1" value="#ffd000" oninput="liveParamChanged('LED')" style="flex:1 1 0; min-width:0; width:100%; height:34px; padding:2px; border:1px solid var(--border-color); border-radius:4px; background:var(--surface-color); cursor:pointer; display:none;">
                            <input type="color" id="paramColourLED2" value="#ff7000" oninput="liveParamChanged('LED')" style="flex:1 1 0; min-width:0; width:100%; height:34px; padding:2px; border:1px solid var(--border-color); border-radius:4px; background:var(--surface-color); cursor:pointer; display:none;">
                            <input type="color" id="paramColourLED3" value="#ff0000" oninput="liveParamChanged('LED')" style="flex:1 1 0; min-width:0; width:100%; height:34px; padding:2px; border:1px solid var(--border-color); border-radius:4px; background:var(--surface-color); cursor:pointer; display:none;">
                        </div>
                        <span id="colourHintLED" style="font-size:0.75rem; display:block; color:var(--text-secondary); margin-top:2px;"></span>
                    </td>
                </tr>
                <tr>
                    <td class="name-col">Speed</td>
                    <td>
                        <input type="range" id="paramSpeedLED" min="0.002" max="0.50" step="0.001" value="0.005" oninput="liveParamChanged('LED')">
                        <span id="speedValLED" style="font-size:0.85rem; display:block; color:var(--primary-color);">0.005s</span>
                    </td>
                </tr>
                <tr>
                    <td class="name-col">Quantity</td>
                    <td>
                        <input type="range" id="paramQuantityLED" min="1" max="8" step="1" value="4" oninput="liveParamChanged('LED')">
                        <span id="qtyValLED" style="font-size:0.85rem; display:block; color:var(--primary-color);">4</span>
                    </td>
                </tr>
                <tr>
                    <td class="name-col">Size</td>
                    <td>
                        <input type="range" id="paramSizeLED" min="1" max="10" step="1" value="5" oninput="liveParamChanged('LED')">
                        <span id="sizeValLED" style="font-size:0.85rem; display:block; color:var(--primary-color);">5</span>
                    </td>
                </tr>
                <tr>
                    <td class="name-col">Wrap</td>
                    <td><input type="checkbox" id="paramWrapLED" onchange="liveParamChanged('LED')"></td>
                </tr>
                <tr>
                    <td class="name-col">Invert</td>
                    <td><input type="checkbox" id="paramInvertLED" onchange="liveParamChanged('LED')"></td>
                </tr>
                <tr style="background-color: #1a1a1a;">
                    <td colspan="2" style="text-align: center; font-weight: bold; color: var(--secondary-color); padding: 8px; border-top: 2px solid var(--secondary-color);">VU Meter Parameters</td>
                </tr>
                <tr>
                    <td class="name-col">Attack LED</td>
                    <td>
                        <input type="range" id="paramAttackLed_LED" min="0.01" max="1.0" step="0.01" value="0.10" oninput="liveParamChanged('LED')">
                        <span id="attLedValLED" style="font-size:0.85rem; display:block; color:var(--primary-color);">0.10s</span>
                    </td>
                </tr>
                <tr>
                    <td class="name-col">Decay LED</td>
                    <td>
                        <input type="range" id="paramDecayLed_LED" min="0.01" max="1.0" step="0.01" value="0.30" oninput="liveParamChanged('LED')">
                        <span id="decLedValLED" style="font-size:0.85rem; display:block; color:var(--primary-color);">0.30s</span>
                    </td>
                </tr>
                <tr style="background-color: #1a1a1a;">
                    <td colspan="2" style="text-align: center; font-weight: bold; color: var(--secondary-color); padding: 8px; border-top: 2px solid var(--secondary-color);">Audio Parameters</td>
                </tr>
                <tr>
                    <td class="name-col">Music Source</td>
                    <td>
                        <select id="paramMusicSourceLED" style="width: 100%;" onchange="liveParamChanged('LED')">
                            <option value="BASS">Bass</option>
                            <option value="FULL RANGE">Full Range</option>
                        </select>
                    </td>
                </tr>
                <tr>
                    <td class="name-col">Frequency</td>
                    <td>
                        <input type="range" id="paramFrequencyLED" min="20" max="100" step="1" value="60" oninput="liveParamChanged('LED')">
                        <span id="freqValLED" style="font-size:0.85rem; display:block; color:var(--primary-color);">60Hz</span>
                    </td>
                </tr>
                <tr>
                    <td class="name-col">Beat Multiplier</td>
                    <td>
                        <input type="range" id="paramBeatMultLED" min="1" max="4" step="1" value="1" oninput="liveParamChanged('LED')">
                        <span id="beatValLED" style="font-size:0.85rem; display:block; color:var(--primary-color);">1x</span>
                    </td>
                </tr>
            </tbody>
        </table>

        <button class="btn" style="margin-top: 15px;" onclick="savePatternConfig('LED')">Save Pattern Settings</button>
        <button id="playlistActionButtonLED" class="btn" style="margin-top: 8px; background-color: var(--primary-color); color: #000;" onclick="handlePlaylistAction('LED')">Save to Playlist</button>
    </div>

    <!-- PLAYLIST TAB -->
    <div id="tab-playlist" class="card">
        <h3>Paired Playlist Builder</h3>
        <div id="playlistContainer" style="margin-top: 5px;"></div>
        <button class="btn" style="margin-top: 15px;" onclick="savePlaylistData()">Save Playlist File</button>
    </div>

    <!-- TIMELINE TAB -->
    <div id="tab-timeline" class="card">
        <h3>Show Timeline Sequencer</h3>
        <div class="control-group">
            <label>Time Division Delay (Seconds: <span id="timelineDelayVal" style="color:var(--primary-color);">20</span>s)</label>
            <input type="range" id="timelineDelaySlider" min="10" max="60" step="1" value="20" oninput="updateTimelineDelayText()">
        </div>
        <p style="font-size: 0.85rem; color: var(--text-secondary);">Columns 1–10 represent LED strips. Column 11 represents the 240V channel. Tap or drag items onto empty slots.</p>
        
        <div class="timeline-wrapper">
            <div id="timelineGridContainer" class="timeline-grid">
                <!-- Dynamically generated via JS -->
            </div>
        </div>
        <button class="btn" style="margin-top: 15px;" onclick="saveTimelineData()">Save Timeline Sequencer</button>
    </div>

    <!-- Floating element for active pickup/drag representation -->
    <div id="floatingItem"></div>

<script>
    function switchTab(tabId) {
        document.querySelectorAll('.card').forEach(c => c.classList.remove('active'));
        document.querySelectorAll('.tab-btn').forEach(b => b.classList.remove('active'));
        document.getElementById('tab-' + tabId).classList.add('active');
        event.currentTarget.classList.add('active');

        if (tabId === 'calibrate') {
            sendCalibrationRequest();
        } else {
            // Leaving the Colours page: STOP the calibration test. It is otherwise
            // latched on the device, so the strip stayed lit showing the last
            // tested order every time the page was opened.
            fetch('/calibrate-stop').catch(() => {});
        }
        if (tabId === 'ws2811') {
            setTimeout(resizeCanvas, 50);
        }
        if (tabId === 'timeline') {
            renderTimelineGrid();
        }
    }

    let colorOrders = ['RGB', 'GRB', 'BRG', 'RBG', 'GBR', 'BGR'];
    let storedCalLoaded = false;   // the saved order is fetched once, from /config
    let userCycled = false;        // true once TEST has been pressed: stop /config
                                   // overwriting orderIndex after the user chose
    let calSessionActive = false;  // true while a test is lit: next TEST cycles

    let orderIndex = 0;

    // TEST: ask the DEVICE what is stored for this port and show it.
    //
    // This used to cycle the page's own orderIndex and light that, so TEST always
    // showed the order AFTER the stored one - confirming a save by displaying the
    // wrong colour. The device owns the stored value, so the device shows it.
    // Both buttons must name the port currently selected. Called by the port
    // dropdown AND by every calibration entry point - it used to be updated only
    // inside the functions the TEST button called, so changing the port left both
    // labels showing the old port number.
    function updatePortLabels() {
        let port = document.getElementById('calPort').value;
        let n = 'PORT ' + (parseInt(port) + 1);
        let t = document.getElementById('testPortNum');
        if (t) t.innerText = n;
        let s = document.getElementById('savePortNum');
        if (s) s.innerText = n;
        calSessionActive = false;   // new port: next TEST shows the stored value
        // BLANK THE STRIPS ON PORT CHANGE. Without this the previous port's test
        // pixels stayed lit, so they looked like the new port's colours.
        fetch('/calibrate-stop').catch(() => {});
        note('Port selected: ' + n);
    }

    // ONE BUTTON, TWO JOBS:
    //   1st press  -> show the STORED value for this port (confirms a save)
    //   later presses -> cycle to the next order (find a new one)
    // The stored value is shown FIRST, which is what makes it a confirmation rather
    // than the old behaviour of always advancing past it.
    function testPortPressed() {
        if (!calSessionActive) {
            calSessionActive = true;
            showStoredCalibration();
        } else {
            cycleColorOrder();
        }
    }

    function showStoredCalibration() {
        let port = document.getElementById('calPort').value;
        note('TEST pressed: asking the device for the stored order of port ' + (parseInt(port) + 1));
        fetch(`/calibrate-show?port=${port}`)
            .then(r => r.text())
            .then(data => note('TEST result: ' + data))
            .catch(e => note('TEST FAILED -> ' + e.message));
    }

    function cycleColorOrder() {
        userCycled = true;      // stop the async /config fetch overwriting our choice
        orderIndex = (orderIndex + 1) % colorOrders.length;
        note('CYCLE: now sending order=' + orderIndex + ' (' + colorOrders[orderIndex] + ')');
        sendCalibrationTest();
    }

    // Called on tab entry and on port change. Updates the port name and loads the
    // stored order, but does NOT light the strip - the strip must only light when
    // the TEST button is pressed.
    function sendCalibrationRequest() {
        let port = document.getElementById('calPort').value;
        // BOTH buttons name the port being tested / saved.
        let lbl = document.getElementById('testPortNum');
        if (lbl) lbl.innerText = 'PORT ' + (parseInt(port) + 1);
        let slbl = document.getElementById('savePortNum');
        if (slbl) slbl.innerText = 'PORT ' + (parseInt(port) + 1);

        if (!storedCalLoaded) {
            storedCalLoaded = true;
            fetch('/config').then(r => r.json()).then(cfg => {
                if (cfg && cfg.portCal && cfg.portCal[port] !== undefined) {
                    orderIndex = cfg.portCal[port];
                }
            }).catch(() => {});
        }
    }

    function sendCalibrationTest() {
        let port = document.getElementById('calPort').value;
        note('calibrate-test port=' + port + ' order=' + orderIndex + ' ...');
        fetch(`/calibrate-test?port=${port}&order=${orderIndex}`)
            .then(response => response.text())
            .then(data => note('calibrate OK -> ' + data))
            .catch(err => note('calibrate FAILED -> ' + err.message));
    }

    function sendCalibrationRequest() {
        let port = document.getElementById('calPort').value;
        // Show which port the button is testing.
        let lbl = document.getElementById('testPortNum');
        if (lbl) lbl.innerText = 'PORT ' + (parseInt(port) + 1);

        // SHOW THE STORED ORDER, not an assumed RGB.
        //
        // The page used to start at orderIndex = 0 every time and never read the
        // saved value. So cycling to the order that made the strip render RGB, then
        // reloading or saving later, stored 0 (RGB = no reorder) instead - and on
        // the next power cycle the strip showed its raw wiring, which is the
        // "powers up showing RBG" symptom. The stored value is what the hardware
        // already applies, so the UI must display the same number.
        if (!storedCalLoaded) {
            storedCalLoaded = true;
            fetch('/config').then(r => r.json()).then(cfg => {
                // ONLY apply the stored value if the user has NOT cycled yet.
                //
                // This fetch is ASYNC, so it could return AFTER the user had already
                // pressed TEST and cycled to the order that matched - silently
                // overwriting orderIndex with the old stored value. SAVE then stored
                // that overwritten value, which is why a value that rendered RGB
                // during testing came back as BRG after a power cycle.
                if (!userCycled && cfg && cfg.portCal && cfg.portCal[port] !== undefined) {
                    orderIndex = cfg.portCal[port];
                    note('STARTUP: stored order for port ' + (parseInt(port) + 1) + ' = ' + orderIndex + ' (' + colorOrders[orderIndex] + ')');
                } else if (userCycled) {
                    note('STARTUP: /config ignored - you have already cycled');
                }
                sendCalibrationRequest();
            }).catch(() => {});
            return;
        }

        // *** NO TEST IS SENT HERE. ***
        //
        // This function is called by switchTab('calibrate'), so anything that sends
        // /calibrate-test in it lights the three calibration pixels the instant the
        // Colours page is selected.
        //
        // The original body ENDED with exactly that send. When the new guarded logic
        // was added ABOVE it, the old tail was left in place - so it ran on every
        // call, including the recursive one from the /config callback. That is the
        // strip lighting up on the Colours page, and it is why every firmware-side
        // fix made no difference: the PAGE was sending the test.
        //
        // The test is sent ONLY by sendCalibrationTest(), which only cycleColorOrder()
        // calls - i.e. only when the TEST button is pressed.
    }

    function saveCalibration() {
        let port = document.getElementById('calPort').value;
        // SAVE WHAT IS ON THE STRIP, not the page's orderIndex.
        //
        // Sending orderIndex overwrote a correct stored value with stale page
        // state: TEST showed RGB (the stored value), then SAVE wrote the page's
        // unrelated index, so the flash showed BRG and the port came back wrong.
        // The device saves its own calOrder, which is exactly what is illuminated.
        note('SAVE pressed: saving what is on the strip for port ' + (parseInt(port) + 1));
        fetch(`/calibrate-savecurrent?port=${port}`)
            .then(response => response.text())
            .then(data => alert(data))
            .catch(err => alert('Error saving calibration: ' + err));
    }

    let currentTheme = 'Disco';
    // ALL themes now have exactly eight colours, so this is a constant. The
    // "Number of Colors" selector and its setColorCount/updateCountButtons plumbing
    // were removed with it.
    const activeColorCount = 8;

    // ORIGINAL theme colours, restored exactly as they were before I changed them.
    //
    // Christmas, Halloween and Animations each have FOUR colours plus four blacks,
    // which is how they have always been. All eight slots are stored and persisted
    // INDIVIDUALLY, so you can duplicate or extend them yourself - the theme does
    // not decide that for you. The black slots need the white ring to be visible,
    // which is why the swatch row borders them.
    const themePresets = {
        'Disco':      { max: 8, colors: ['#FF0000', '#00FF00', '#0000FF', '#FFFF00', '#800080', '#00FFFF', '#FFFFFF', '#FFA500'] },
        'Christmas':  { max: 8, colors: ['#FF0000', '#228B22', '#87CEFA', '#FFD700', '#000000', '#000000', '#000000', '#000000'] },
        'Halloween':  { max: 8, colors: ['#FF0000', '#00FF00', '#4B0082', '#FFA500', '#000000', '#000000', '#000000', '#000000'] },
        'Animations': { max: 8, colors: ['#FF4500', '#00CED1', '#FF1493', '#ADFF2F', '#000000', '#000000', '#000000', '#000000'] }
    };

    // Suppress the theme preview while the page is first loading so the
    // strips stay dark until the user actually makes a choice.
    let themePreviewEnabled = false;

    function setTheme(theme) {
        currentTheme = theme;
        document.querySelectorAll('.theme-btn').forEach(b => {
            b.classList.toggle('active', b.innerText === theme);
        });

        loadThemeColorsToUI();
        buildColorBoxes();
        if (themePreviewEnabled) sendThemePreview();
    }

    function loadThemeColorsToUI() {
        let presetColors = themePresets[currentTheme].colors;
        for (let i = 0; i < 8; i++) {
            let input = document.getElementById('colorInput-' + i);
            if (input) input.value = presetColors[i];
        }
    }

    let previewTimeout = null;
    function buildColorBoxes() {
        let container = document.getElementById('colorBoxesContainer');
        container.innerHTML = '';
        let presetColors = themePresets[currentTheme].colors;

        for (let i = 0; i < 8; i++) {
            let slot = document.createElement('div');
            slot.className = 'slot-box';
            slot.id = 'colorSlot-' + i;
            
            let input = document.createElement('input');
            input.type = 'color';
            input.value = presetColors[i];
            input.id = 'colorInput-' + i;
            
            input.oninput = function() {
                themePresets[currentTheme].colors[i] = this.value;
                clearTimeout(previewTimeout);
                previewTimeout = setTimeout(() => { sendThemePreview(); }, 250);
            };
            
            slot.appendChild(input);
            container.appendChild(slot);
        }
        updateColorBoxesState();
    }

    function updateColorBoxesState() {
        for (let i = 0; i < 8; i++) {
            let slot = document.getElementById('colorSlot-' + i);
            if (slot) {
                if (i < activeColorCount) slot.classList.remove('disabled');
                else slot.classList.add('disabled');
            }
        }
    }

    function sendThemePreview() {
        let colors = [];
        for (let i = 0; i < activeColorCount; i++) {
            colors.push(document.getElementById('colorInput-' + i).value);
        }
        fetch('/theme-preview', {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify({ theme: currentTheme, count: activeColorCount, colors: colors })
        })
        .then(r => note('theme OK ' + r.status))
        .catch(e => note('theme FAILED -> ' + e.message));
    }

    function saveThemePalette() {
        sendThemePreview();
        triggerSaveFlash();
        alert('Theme palette saved successfully!');
    }

    // Pattern state tracking per section
    let selectedPatternName240 = '';
    let selectedPatternNameLED = '';
    let currentLayoutLED = 'STAR';
    let playlistItems = [];
    let patternChosen = false;

    // Timeline Data Structures
    let timelineRowsCount = 8;
    let timelineData = {}; // key: "row_col", value: playlistItemObj
    let activeArmedItem = null; // Holds item being placed or moved

    function choosePattern(patternName, el) {
        selectedPatternName240 = patternName;
        selectedPatternNameLED = patternName;
        patternChosen = true;

        document.querySelectorAll('.pattern-item').forEach(item => item.classList.remove('active'));
        if (el) el.classList.add('active');

        document.getElementById('configPatternTitle240').innerText = patternName;
        document.getElementById('configPatternTitleLED').innerText = patternName;

        document.getElementById('tabBtn240').removeAttribute('disabled');
        document.getElementById('tabBtnLED').removeAttribute('disabled');

        // Tell the ESP32 which pattern was chosen so the displays can start.
        // Outputs stay dark until a pattern is selected.
        liveParamChanged('240V');
        liveParamChanged('LED');
    }

    function updateLivePreviewLayout(section) {
        if (section === 'LED') {
            currentLayoutLED = document.getElementById('patternLayoutSelectLED').value;
        }
        liveParamChanged(section);
    }

    // Read-out labels only - no request to the ESP32.
    function updateParamLabels(section) {
        let suffix = (section === '240V') ? '240' : 'LED';
        document.getElementById('speedVal' + suffix).innerText = parseFloat(document.getElementById('paramSpeed' + suffix).value).toFixed(3) + 's';
        // Beat sensitivity is global (no section suffix).
        let bsEl = document.getElementById('paramBeatSens');
        let bsLbl = document.getElementById('beatSensVal');
        if (bsEl && bsLbl) bsLbl.innerText = parseFloat(bsEl.value).toFixed(2);
        document.getElementById('qtyVal' + suffix).innerText = document.getElementById('paramQuantity' + suffix).value;
        document.getElementById('sizeVal' + suffix).innerText = document.getElementById('paramSize' + suffix).value;
        document.getElementById('freqVal' + suffix).innerText = document.getElementById('paramFrequency' + suffix).value + 'Hz';
        document.getElementById('beatVal' + suffix).innerText = document.getElementById('paramBeatMult' + suffix).value + 'x';

        if (section === '240V') {
            document.getElementById('att240Val240').innerText = parseFloat(document.getElementById('paramAttack240_240').value).toFixed(2) + 's';
            document.getElementById('dec240Val240').innerText = parseFloat(document.getElementById('paramDecay240_240').value).toFixed(2) + 's';
        } else {
            document.getElementById('attLedValLED').innerText = parseFloat(document.getElementById('paramAttackLed_LED').value).toFixed(2) + 's';
            document.getElementById('decLedValLED').innerText = parseFloat(document.getElementById('paramDecayLed_LED').value).toFixed(2) + 's';
        }
    }

    // Push the values saved on the ESP32 back into the sliders after a reload.
    // Deliberately does NOT send a live-tweak: the outputs must stay dark until
    // a pattern is selected.
    function restoreParams(cfg) {
        const set = (id, v) => { let e = document.getElementById(id); if (e && v !== undefined && v !== null) e.value = v; };
        const chk = (id, v) => { let e = document.getElementById(id); if (e && v !== undefined) e.checked = !!v; };

        set('paramSpeed240', cfg.speed240);   set('paramSpeedLED', cfg.speedLED);
        set('paramBeatSens', cfg.beatSens);   // global
        // Restore the saved LED colours. Guarded: an older saved config has no
        // colourLED array, and we must not blank the inputs in that case.
        if (cfg.colourLED && cfg.colourLED.length) {
            for (let i = 0; i < 4 && i < cfg.colourLED.length; i++) {
                let el = document.getElementById('paramColourLED' + i);
                if (el && /^#[0-9a-f]{6}$/i.test(cfg.colourLED[i])) el.value = cfg.colourLED[i];
            }
        }
        updateColourBoxes('LED');
        set('paramQuantity240', cfg.qty240);  set('paramQuantityLED', cfg.qtyLED);
        set('paramSize240', cfg.size240);     set('paramSizeLED', cfg.sizeLED);
        chk('paramWrap240', cfg.wrap240);     chk('paramWrapLED', cfg.wrapLED);
        chk('paramInvert240', cfg.inv240);    chk('paramInvertLED', cfg.invLED);
        set('paramAttack240_240', cfg.atk240); set('paramAttackLed_LED', cfg.atkLED);
        set('paramDecay240_240', cfg.dec240);  set('paramDecayLed_LED', cfg.decLED);
        set('paramMusicSource240', cfg.music240); set('paramMusicSourceLED', cfg.musicLED);
        set('paramFrequency240', cfg.freq240); set('paramFrequencyLED', cfg.freqLED);
        set('paramBeatMult240', cfg.mult240);  set('paramBeatMultLED', cfg.multLED);

        set('antifreeze', cfg.antifreeze);

        updateParamLabels('240V');
        updateParamLabels('LED');
    }

    function liveParamChanged(section) {
        updateParamLabels(section);
        updateColourBoxes(section);

        clearTimeout(window.liveTimer);
        window.liveTimer = setTimeout(() => {
            fetch('/live-tweak', {
                method: 'POST',
                headers: { 'Content-Type': 'application/json' },
                body: JSON.stringify(collectCurrentParameters(section))
            })
            .then(r => note('tweak OK ' + r.status + ' (' + section + ')'))
            .catch(e => note('tweak FAILED -> ' + e.message));
        }, 100);
    }

    function collectCurrentParameters(section) {
        let suffix = (section === '240V') ? '240' : 'LED';
        let pName = (section === '240V') ? selectedPatternName240 : selectedPatternNameLED;
        let pLayout = (section === '240V') ? 'STAR' : currentLayoutLED;
        return {
            name: pName,
            section: section,
            layout: pLayout,
            speed: document.getElementById('paramSpeed' + suffix).value,
            quantity: document.getElementById('paramQuantity' + suffix).value,
            size: document.getElementById('paramSize' + suffix).value,
            wrap: document.getElementById('paramWrap' + suffix).checked,
            invert: document.getElementById('paramInvert' + suffix).checked,
            attack: (section === '240V') ? document.getElementById('paramAttack240_240').value : document.getElementById('paramAttackLed_LED').value,
            decay: (section === '240V') ? document.getElementById('paramDecay240_240').value : document.getElementById('paramDecayLed_LED').value,
            musicSource: document.getElementById('paramMusicSource' + suffix).value,
            frequency: document.getElementById('paramFrequency' + suffix).value,
            beatMult: document.getElementById('paramBeatMult' + suffix).value,
            // Global beat-detection sensitivity (one detector, both displays), so
            // it is sent with either section.
            beatSens: document.getElementById('paramBeatSens').value,
            // Only the LED section has colour. Omitted (undefined) for 240V, which
            // JSON.stringify drops, so the backend array parse never sees it.
            colour: (section === '240V') ? undefined
                    : [0, 1, 2, 3].map(i => document.getElementById('paramColourLED' + i).value)
        };
    }

    // A VU pattern uses four colour boxes; every other LED pattern uses one.
    // The boxes are flex siblings in a fixed-width row, so four boxes share
    // exactly the space the single box occupies - the row length never changes.
    function isVuPattern() {
        let sel = document.getElementById('patternLayoutSelectLED');
        if (!sel) return false;
        let txt = (sel.value || '') + ' ' +
                  (sel.options && sel.selectedIndex >= 0 ? sel.options[sel.selectedIndex].text : '');
        return /vu/i.test(txt) || /vu/i.test(selectedPatternNameLED || '');
    }

    function updateColourBoxes(section) {
        if (section === '240V') return;
        let four = isVuPattern();
        for (let i = 0; i < 4; i++) {
            let el = document.getElementById('paramColourLED' + i);
            if (el) el.style.display = (i === 0 || four) ? '' : 'none';
        }
        let hint = document.getElementById('colourHintLED');
        if (hint) hint.innerText = four ? '4 bands: low \u2192 peak' : '';
    }

    function getLayoutSvgIcon(layout) {
        let svgNS = "http://www.w3.org/2000/svg";
        let svg = document.createElementNS(svgNS, "svg");
        svg.setAttribute("width", "24");
        svg.setAttribute("height", "24");
        svg.setAttribute("viewBox", "0 0 24 24");
        svg.style.display = "block";
        svg.style.flexShrink = "0";

        let strokeColor = "var(--text-secondary)";
        let strokeWidth = "1";

        if (layout === 'STAR') {
            let cx = 12, cy = 12, r = 10;
            for (let i = 0; i < 10; i++) {
                let angle = (i / 10) * Math.PI * 2;
                let line = document.createElementNS(svgNS, "line");
                line.setAttribute("x1", cx);
                line.setAttribute("y1", cy);
                line.setAttribute("x2", cx + Math.cos(angle) * r);
                line.setAttribute("y2", cy + Math.sin(angle) * r);
                line.setAttribute("stroke", strokeColor);
                line.setAttribute("stroke-width", strokeWidth);
                line.setAttribute("stroke-linecap", "round");
                svg.appendChild(line);
            }
        } else if (layout === 'GRID HORIZONTAL') {
            for (let i = 0; i < 10; i++) {
                let y = 2.5 + i * 2.1;
                let line = document.createElementNS(svgNS, "line");
                line.setAttribute("x1", "2");
                line.setAttribute("y1", y);
                line.setAttribute("x2", "22");
                line.setAttribute("y2", y);
                line.setAttribute("stroke", strokeColor);
                line.setAttribute("stroke-width", strokeWidth);
                line.setAttribute("stroke-linecap", "round");
                svg.appendChild(line);
            }
        } else if (layout === 'GRID VERTICAL') {
            for (let i = 0; i < 10; i++) {
                let x = 2.5 + i * 2.1;
                let line = document.createElementNS(svgNS, "line");
                line.setAttribute("x1", x);
                line.setAttribute("y1", "2");
                line.setAttribute("x2", x);
                line.setAttribute("y2", "22");
                line.setAttribute("stroke", strokeColor);
                line.setAttribute("stroke-width", strokeWidth);
                line.setAttribute("stroke-linecap", "round");
                svg.appendChild(line);
            }
        } else if (layout === 'LINE') {
            let rect = document.createElementNS(svgNS, "rect");
            rect.setAttribute("x", "3");
            rect.setAttribute("y", "6");
            rect.setAttribute("width", "18");
            rect.setAttribute("height", "12");
            rect.setAttribute("rx", "6");
            rect.setAttribute("ry", "6");
            rect.setAttribute("fill", "none");
            rect.setAttribute("stroke", strokeColor);
            rect.setAttribute("stroke-width", strokeWidth);
            svg.appendChild(rect);
        } else {
            let line = document.createElementNS(svgNS, "line");
            line.setAttribute("x1", "2");
            line.setAttribute("y1", "12");
            line.setAttribute("x2", "22");
            line.setAttribute("y2", "12");
            line.setAttribute("stroke", strokeColor);
            line.setAttribute("stroke-width", strokeWidth);
            svg.appendChild(line);
        }
        return svg;
    }

    function savePatternConfig(section) {
        let params = collectCurrentParameters(section);
        triggerSaveFlash();
        alert(`Settings saved for ${params.name} (${section})`);
    }

    function handlePlaylistAction(section) {
        let patternConfig = collectCurrentParameters(section);
        playlistItems.push(patternConfig);
        alert(`"${patternConfig.name}" (${section}) added to playlist!`);
        renderPlaylist();
        triggerSaveFlash();
    }

    function addPlaylistToTimeline(index) {
        activeArmedItem = JSON.parse(JSON.stringify(playlistItems[index]));
        showFloatingItem(activeArmedItem.name);
        switchTabByName('timeline');
    }

    function switchTabByName(tabId) {
        document.querySelectorAll('.card').forEach(c => c.classList.remove('active'));
        document.querySelectorAll('.tab-btn').forEach(b => {
            if (b.getAttribute('onclick')?.includes(tabId)) b.classList.add('active');
            else b.classList.remove('active');
        });
        document.getElementById('tab-' + tabId).classList.add('active');
        if (tabId === 'timeline') renderTimelineGrid();
    }

    function renderPlaylist() {
        let container = document.getElementById('playlistContainer');
        container.innerHTML = '';

        if (playlistItems.length === 0) {
            container.innerHTML = '<p style="color: var(--text-secondary); text-align: center;">Playlist is empty. Add patterns from 240V or LED Display tabs.</p>';
            return;
        }

        playlistItems.forEach((item, index) => {
            let div = document.createElement('div');
            div.className = 'playlist-item';

            let indexSpan = document.createElement('span');
            indexSpan.className = 'playlist-item-index';
            indexSpan.innerText = `${index + 1}.`;
            div.appendChild(indexSpan);

            let iconContainer = document.createElement('div');
            iconContainer.title = `Layout: ${item.layout || 'STAR'}`;
            iconContainer.style.display = 'flex';
            iconContainer.style.alignItems = 'center';
            iconContainer.style.justifyContent = 'center';
            iconContainer.appendChild(getLayoutSvgIcon(item.layout || 'STAR'));
            div.appendChild(iconContainer);

            let nameInput = document.createElement('input');
            nameInput.type = 'text';
            nameInput.value = `${item.name} [${item.section || '240V'}]`;
            nameInput.oninput = (e) => { item.name = e.target.value; };
            div.appendChild(nameInput);

            let actionsDiv = document.createElement('div');
            actionsDiv.className = 'playlist-actions';

            let timelineAddBtn = document.createElement('button');
            timelineAddBtn.className = 'playlist-btn timeline-add-btn';
            timelineAddBtn.innerText = '+';
            timelineAddBtn.title = 'Add to Timeline';
            timelineAddBtn.onclick = () => addPlaylistToTimeline(index);
            actionsDiv.appendChild(timelineAddBtn);

            if (playlistItems.length > 1) {
                let upBtn = document.createElement('button');
                upBtn.className = 'playlist-btn';
                upBtn.innerText = '▲';
                upBtn.disabled = (index === 0);
                upBtn.onclick = () => movePlaylistItem(index, -1);
                actionsDiv.appendChild(upBtn);

                let downBtn = document.createElement('button');
                downBtn.className = 'playlist-btn';
                downBtn.innerText = '▼';
                downBtn.disabled = (index === playlistItems.length - 1);
                downBtn.onclick = () => movePlaylistItem(index, 1);
                actionsDiv.appendChild(downBtn);
            }

            let deleteBtn = document.createElement('button');
            deleteBtn.className = 'playlist-btn delete-btn';
            deleteBtn.innerText = '✕';
            deleteBtn.title = 'Delete item';
            deleteBtn.onclick = () => deletePlaylistItem(index);
            actionsDiv.appendChild(deleteBtn);

            div.appendChild(actionsDiv);
            container.appendChild(div);
        });
    }

    function movePlaylistItem(index, direction) {
        let newIndex = index + direction;
        if (newIndex >= 0 && newIndex < playlistItems.length) {
            let temp = playlistItems[index];
            playlistItems[index] = playlistItems[newIndex];
            playlistItems[newIndex] = temp;
            renderPlaylist();
        }
    }

    function deletePlaylistItem(index) {
        playlistItems.splice(index, 1);
        renderPlaylist();
    }

    function loadPlaylistFromServer() {
        fetch('/load-playlist')
            .then(response => response.json())
            .then(data => {
                if (data && data.items) {
                    playlistItems = data.items;
                    renderPlaylist();
                }
            })
            .catch(err => console.log('No saved playlist found'));
    }

    function savePlaylistData() {
        fetch('/save-playlist', {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify({ items: playlistItems })
        })
        .then(() => {
            triggerSaveFlash();
            alert('Playlist saved successfully to LittleFS!');
        })
        .catch(err => alert('Error saving playlist: ' + err));
    }

    // --- TIMELINE BUILDER LOGIC ---
    function updateTimelineDelayText() {
        let val = document.getElementById('timelineDelaySlider').value;
        document.getElementById('timelineDelayVal').innerText = val;
    }

    function renderTimelineGrid() {
        let container = document.getElementById('timelineGridContainer');
        container.innerHTML = '';

        // Header Row: Time + 10 Strips + 240V
        let timeTh = document.createElement('div');
        timeTh.className = 'timeline-header-cell';
        timeTh.innerText = 'Time';
        container.appendChild(timeTh);

        for (let i = 1; i <= 10; i++) {
            let th = document.createElement('div');
            th.className = 'timeline-header-cell';
            th.innerText = `S${i}`;
            container.appendChild(th);
        }

        let th240 = document.createElement('div');
        th240.className = 'timeline-header-cell';
        th240.innerText = '240V';
        container.appendChild(th240);

        // Grid Rows
        let delaySec = parseInt(document.getElementById('timelineDelaySlider').value);

        for (let r = 0; r < timelineRowsCount; r++) {
            let timeLabel = document.createElement('div');
            timeLabel.className = 'timeline-row-label';
            let startT = r * delaySec;
            let endT = (r + 1) * delaySec;
            timeLabel.innerText = `${startT}-${endT}s`;
            container.appendChild(timeLabel);

            for (let c = 1; c <= 11; c++) {
                let cell = document.createElement('div');
                cell.className = 'timeline-cell';
                let cellKey = `${r}_${c}`;
                cell.dataset.row = r;
                cell.dataset.col = c;

                let item = timelineData[cellKey];
                if (item) {
                    let block = document.createElement('div');
                    block.className = 'timeline-block';
                    
                    let title = document.createElement('div');
                    title.className = 'timeline-block-title';
                    title.innerText = item.name;
                    block.appendChild(title);

                    let sub = document.createElement('div');
                    sub.style.fontSize = '0.65rem';
                    sub.style.color = 'var(--secondary-color)';
                    sub.innerText = c === 11 ? '240V' : `Strip ${c}`;
                    block.appendChild(sub);

                    let delBtn = document.createElement('button');
                    delBtn.className = 'timeline-block-delete';
                    delBtn.innerText = '✕';
                    delBtn.onclick = (e) => {
                        e.stopPropagation();
                        delete timelineData[cellKey];
                        renderTimelineGrid();
                    };
                    block.appendChild(delBtn);

                    // Allow picking up existing item to move/reposition via tap or drag
                    block.onpointerdown = (e) => {
                        e.stopPropagation();
                        activeArmedItem = item;
                        delete timelineData[cellKey];
                        showFloatingItem(activeArmedItem.name);
                        renderTimelineGrid();
                    };

                    cell.appendChild(block);
                }

                // Cell click / tap / push handler for universal placement
                cell.onpointerup = (e) => {
                    if (activeArmedItem) {
                        let isPressureDrop = e.pressure && e.pressure > 0.5;
                        if (!timelineData[cellKey] || isPressureDrop) {
                            timelineData[cellKey] = activeArmedItem;
                            activeArmedItem = null;
                            hideFloatingItem();
                            renderTimelineGrid();
                        }
                    }
                };

                container.appendChild(cell);
            }
        }
    }

    function showFloatingItem(text) {
        let fi = document.getElementById('floatingItem');
        fi.innerText = `Placing: ${text}`;
        fi.style.display = 'block';
    }

    function hideFloatingItem() {
        let fi = document.getElementById('floatingItem');
        fi.style.display = 'none';
    }

    window.addEventListener('pointermove', (e) => {
        let fi = document.getElementById('floatingItem');
        if (fi && fi.style.display === 'block') {
            fi.style.left = e.clientX + 'px';
            fi.style.top = e.clientY + 'px';

            // Highlight target cell under cursor
            let elem = document.elementFromPoint(e.clientX, e.clientY);
            document.querySelectorAll('.timeline-cell').forEach(c => {
                c.classList.remove('highlight-valid', 'highlight-invalid');
            });
            if (elem && elem.classList.contains('timeline-cell')) {
                let r = elem.dataset.row;
                let c = elem.dataset.col;
                if (timelineData[`${r}_${c}`]) {
                    elem.classList.add('highlight-invalid');
                } else {
                    elem.classList.add('highlight-valid');
                }
            }
        }
    });

    window.addEventListener('pointerup', () => {
        document.querySelectorAll('.timeline-cell').forEach(c => {
            c.classList.remove('highlight-valid', 'highlight-invalid');
        });
    });

    function saveTimelineData() {
        let delaySec = document.getElementById('timelineDelaySlider').value;
        fetch('/save-timeline', {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify({ delay: delaySec, items: timelineData })
        })
        .then(() => {
            triggerSaveFlash();
            alert('Timeline saved successfully!');
        })
        .catch(err => alert('Error saving timeline: ' + err));
    }

    function triggerSaveFlash() {
        fetch('/save-flash', { method: 'POST' }).catch(err => {});
    }

    // --- CANVAS PREVIEW ANIMATION ENGINE ---
    function resizeCanvas() {
        let c = document.getElementById('livePreviewCanvasLED');
        if (c && c.parentElement) {
            c.width = c.parentElement.clientWidth - 20;
            c.height = 220;
        }
    }
    window.onresize = resizeCanvas;

    let animStep = 0;
    function renderPreviewLoop() {
        animStep += 0.04;
        let cardId = 'tab-ws2811';
        let canvasId = 'livePreviewCanvasLED';
        
        if (document.getElementById(cardId).classList.contains('active')) {
            let canvas = document.getElementById(canvasId);
            if (canvas) {
                let ctx = canvas.getContext('2d');
                let layout = currentLayoutLED;

                ctx.fillStyle = '#050505';
                ctx.fillRect(0, 0, canvas.width, canvas.height);

                ctx.font = '9px sans-serif';
                ctx.fillStyle = '#888';

                if (layout === 'STAR') {
                    let cx = canvas.width / 2;
                    let cy = canvas.height / 2;
                    let maxRadius = Math.min(cx, cy) - 18;

                    ctx.fillStyle = '#444';
                    ctx.beginPath(); ctx.arc(cx, cy, 6, 0, Math.PI * 2); ctx.fill();

                    for (let s = 0; s < 10; s++) {
                        let portNum = ((s - 8 + 10) % 10) + 1;
                        let angle = (s / 10) * Math.PI * 2;
                        let activePos = ((Math.sin(animStep + s * 0.7) + 1) / 2) * maxRadius;

                        ctx.strokeStyle = '#222';
                        ctx.lineWidth = 3;
                        ctx.beginPath();
                        ctx.moveTo(cx, cy);
                        ctx.lineTo(cx + Math.cos(angle) * maxRadius, cy + Math.sin(angle) * maxRadius);
                        ctx.stroke();

                        let x = cx + Math.cos(angle) * activePos;
                        let y = cy + Math.sin(angle) * activePos;
                        ctx.fillStyle = '#03dac6';
                        ctx.beginPath(); ctx.arc(x, y, 1.5, 0, Math.PI * 2); ctx.fill();

                        let labelRadius = maxRadius + 10;
                        let lx = cx + Math.cos(angle) * labelRadius;
                        let ly = cy + Math.sin(angle) * labelRadius;
                        ctx.font = '9px sans-serif';
                        ctx.fillStyle = '#888';
                        ctx.textAlign = 'center';
                        ctx.textBaseline = 'middle';
                        ctx.fillText(portNum, lx, ly);
                    }
                } else if (layout === 'GRID HORIZONTAL') {
                    let rows = 10;
                    let spacing = canvas.height / (rows + 1);
                    for (let r = 0; r < rows; r++) {
                        let y = spacing * (r + 1);
                        ctx.strokeStyle = '#222';
                        ctx.lineWidth = 4;
                        ctx.beginPath();
                        ctx.moveTo(25, y);
                        ctx.lineTo(canvas.width - 15, y);
                        ctx.stroke();

                        let progress = (Math.sin(animStep + r * 0.5) + 1) / 2;
                        let activeX = 25 + progress * (canvas.width - 40);
                        ctx.strokeStyle = '#bb86fc';
                        ctx.lineWidth = 4;
                        ctx.beginPath();
                        ctx.moveTo(activeX - 2, y);
                        ctx.lineTo(activeX + 2, y);
                        ctx.stroke();

                        ctx.font = '9px sans-serif';
                        ctx.fillStyle = '#888';
                        ctx.textAlign = 'right';
                        ctx.textBaseline = 'middle';
                        ctx.fillText(r + 1, 20, y);
                    }
                } else if (layout === 'GRID VERTICAL') {
                    let cols = 10;
                    let spacing = canvas.width / (cols + 1);
                    for (let c = 0; c < cols; c++) {
                        let x = spacing * (c + 1);
                        ctx.strokeStyle = '#222';
                        ctx.lineWidth = 4;
                        ctx.beginPath();
                        ctx.moveTo(x, 20);
                        ctx.lineTo(x, canvas.height - 20);
                        ctx.stroke();

                        let progress = (Math.sin(animStep + c * 0.5) + 1) / 2;
                        let activeY = 20 + progress * (canvas.height - 40);
                        ctx.strokeStyle = '#03dac6';
                        ctx.lineWidth = 4;
                        ctx.beginPath();
                        ctx.moveTo(x, activeY - 2);
                        ctx.lineTo(x, activeY + 2);
                        ctx.stroke();

                        ctx.font = '9px sans-serif';
                        ctx.fillStyle = '#888';
                        ctx.textAlign = 'center';
                        ctx.textBaseline = 'top';
                        ctx.fillText(c + 1, x, canvas.height - 15);
                    }
                } else if (layout === 'LINE') {
                    let rx = 35;
                    let x1 = 50, x2 = canvas.width - 50;
                    let yTop = 70, yBot = 150;

                    ctx.strokeStyle = '#222';
                    ctx.lineWidth = 5;
                    ctx.beginPath();
                    ctx.moveTo(x1, yTop); ctx.lineTo(x2, yTop);
                    ctx.arc(x2, (yTop + yBot)/2, (yBot - yTop)/2, -Math.PI/2, Math.PI/2);
                    ctx.lineTo(x1, yBot);
                    ctx.arc(x1, (yTop + yBot)/2, (yBot - yTop)/2, Math.PI/2, (3*Math.PI)/2);
                    ctx.stroke();

                    let straightLen = x2 - x1;
                    let curveRadius = (yBot - yTop) / 2;
                    let curveLen = Math.PI * curveRadius;
                    let totalLen = (straightLen * 2) + (curveLen * 2);

                    function getPointAtDist(d) {
                        d = d % totalLen;
                        if (d < 0) d += totalLen;
                        let cx = 0, cy = 0;
                        if (d < straightLen) {
                            cx = x1 + d; cy = yTop;
                        } else if (d < straightLen + curveLen) {
                            let angle = -Math.PI/2 + ((d - straightLen) / curveLen) * Math.PI;
                            cx = x2 + Math.cos(angle) * curveRadius;
                            cy = (yTop + yBot)/2 + Math.sin(angle) * curveRadius;
                        } else if (d < (straightLen * 2) + curveLen) {
                            cx = x2 - (d - straightLen - curveLen); cy = yBot;
                        } else {
                            let angle = Math.PI/2 + ((d - (straightLen * 2) - curveLen) / curveLen) * Math.PI;
                            cx = x1 + Math.cos(angle) * curveRadius;
                            cy = (yTop + yBot)/2 + Math.sin(angle) * curveRadius;
                        }
                        return { x: cx, y: cy };
                    }

                    for (let p = 0; p < 10; p++) {
                        let dist = (p / 10) * totalLen;
                        let pt = getPointAtDist(dist);
                        ctx.font = '8px sans-serif';
                        ctx.fillStyle = '#888';
                        ctx.textAlign = 'center';
                        ctx.textBaseline = 'middle';
                        ctx.fillText(p + 1, pt.x, pt.y - 12);
                    }

                    let currentDist = (animStep * 40) % totalLen;
                    let activePt = getPointAtDist(currentDist);

                    ctx.fillStyle = '#ff4500';
                    ctx.beginPath(); ctx.arc(activePt.x, activePt.y, 2.5, 0, Math.PI*2); ctx.fill();
                }
            }
        }
        requestAnimationFrame(renderPreviewLoop);
    }
    requestAnimationFrame(renderPreviewLoop);

    window.onload = function() {
        // Restore the saved theme/colours from the ESP32 so a power cycle
        // does not lose the colour-theme settings.
        fetch('/settings', { cache: 'no-store' })
            .then(r => r.json())
            .then(cfg => {
                let savedTheme = (cfg.theme && themePresets[cfg.theme]) ? cfg.theme : 'Disco';
                if (Array.isArray(cfg.themeColors) && cfg.themeColors.length > 0) {
                    for (let i = 0; i < 8; i++) {
                        themePresets[savedTheme].colors[i] = cfg.themeColors[i] || '#000000';
                    }
                }
                if (cfg.themeCount) {
                    themePresets[savedTheme].max = Math.max(themePresets[savedTheme].max, cfg.themeCount);
                    // (activeColorCount is gone: all themes have eight colours now.)

                }
                if (cfg.brightness !== undefined) {
                    let b = document.getElementById('masterBrightness');
                    if (b) { b.value = cfg.brightness; document.getElementById('brVal').innerText = cfg.brightness; }
                }
                if (cfg.audioEnabled !== undefined) {
                    let a = document.getElementById('audioEnabled');
                    if (a) a.value = cfg.audioEnabled ? 'true' : 'false';
                }
                restoreParams(cfg);
                currentTheme = savedTheme;
                setTheme(savedTheme);
                // themePreviewEnabled is deliberately NOT armed here.
                //
                // Arming it at page startup meant that from that moment ANY
                // setTheme() call posted /theme-preview, which sets previewMode = 2
                // and paints EVERY strip in the theme colours reordered by their
                // calibration. That is the strip lighting up when the Colours page
                // is opened, and why six firmware-side fixes changed nothing: the
                // page kept sending the preview.
                //
                // It is armed on the theme BUTTONS instead, so only an explicit
                // user action previews.
            })
            .catch(() => { setTheme('Disco'); })
            .finally(() => {
                buildColorBoxes();
                loadPlaylistFromServer();
                resizeCanvas();
                // NOTE: do NOT auto-send /init-off here. It used to run on EVERY
                // page load, so any reload (Safari prefetch / tab eviction /
                // screen wake) blanked a running display and made it look like
                // the system had "shut down". The firmware boots dark on its own;
                // use the "All Off" button when you actually want outputs off.
                fetchDiag();
                // Keep the diagnostics live: "NOW UP" restarting from 0 means
                // the board rebooted, and the reason survives the reset.
                setInterval(fetchDiag, 3000);
                updateVuAvailability();
            });
    };

    function note(msg) {
        var e = document.getElementById('netStatus');
        if (e) e.innerText = 'net: ' + msg;
    }

    function setAudioDetect(v) {
        fetch('/audio?enabled=' + v)
            .then(r => { note('audio-detect OK ' + r.status); updateVuAvailability(); })
            .catch(e => note('audio-detect FAILED -> ' + e.message));
    }

    // VU patterns read the audio analyser, so they are meaningless with Audio
    // Detect off: grey them out (and stop them being clickable) while it is off.
    function updateVuAvailability() {
        let a = document.getElementById('audioEnabled');
        let on = !a || a.value === 'true';
        document.querySelectorAll('.pattern-item[data-vu="1"]').forEach(b => {
            b.classList.toggle('vu-disabled', !on);
        });
    }

    // Antifreeze: seconds without a beat before the display is forced to change
    // direction (this also covers Audio Detect being off). Never affects speed.
    function setAntifreeze(v) {
        let n = parseInt(v, 10);
        if (isNaN(n)) n = 2;
        n = Math.max(1, Math.min(9, n));
        fetch('/update', {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify({ antifreeze: n })
        })
            .then(r => note('antifreeze ' + n + 's saved HTTP ' + r.status))
            .catch(e => note('antifreeze FAILED -> ' + e.message));
    }

    // Crash diagnostics. Reports WHY the ESP32's previous run ended, how long it
    // survived and where it was when it died - read from /diag so the serial
    // port never has to be opened (opening it resets the board).
    function fetchDiag() {
        fetch('/diag', { cache: 'no-store' })
            .then(r => r.json())
            .then(j => {
                let last = (j.prevValid === 0)
                    ? 'last run: unknown (RTC lost = real power cycle)'
                    : 'last run: ' + j.prevReasonTx + ' after ' + j.prevUpSec +
                      's (stage ' + j.prevStage + ', heap ' + j.prevHeap + ')';
                let lk = j.leakWhat
                    ? ' | LEAK ' + j.leakWhat + ' p1 ' + j.leakPass1 + 'B p2 ' + j.leakPass2 + 'B'
                    : '';
                let msg = last + ' | UP ' + j.uptimeSec + 's, heap ' + j.freeHeap +
                          ', MIN ' + j.minFreeHeap + ', boots ' + j.boots +
                          ', nvsErase ' + j.erases + ', nvsFree ' + j.nvsFreeEntries + lk;
                let e = document.getElementById('diagInfo');
                if (e) e.innerText = msg;
                note(msg);
            })
            .catch(e => note('diag FAILED -> ' + e.message));
    }

    // Explicit "everything off" (this used to fire automatically on page load).
    function allOff() {
        fetch('/init-off', { method: 'POST' })
            .then(r => note('all outputs OFF HTTP ' + r.status))
            .catch(e => note('all-off FAILED -> ' + e.message));
    }



    // Measures the heap cost of each output path so a memory leak can be
    // attributed: FastLED.show() (RMT), our strip engine, and the 240V engine.
    // PASS 2 is the meaningful number - pass 1 only warms FastLED's RMT buffer
    // pool. The result goes in its own element that the /diag refresh never
    // touches, so it stays on screen to be read.
    async function leakTest() {
        let el = document.getElementById('leakInfo');
        if (el) el.innerText = 'LEAKTEST running (pass1 warms the pool, pass2 measures)...';
        let out = '', minHeap = '';
        for (const w of ['fastled', 'led', 'relay']) {
            try {
                const r = await fetch('/leaktest?what=' + w + '&n=30', { cache: 'no-store' });
                const j = await r.json();
                out += w + ': pass1 ' + j.pass1 + 'B, PASS2 ' + j.pass2 + 'B (' +
                       j.perCall2.toFixed(2) + '/call)   ';
                minHeap = j.minFreeHeap;
            } catch (err) {
                out += w + ': FAILED (' + err.message + ')   ';
            }
        }
        if (el) el.innerText = 'LEAKTEST -> ' + out + '| minFreeHeap ' + minHeap;
        note('LEAKTEST -> ' + out);
    }

    function testConnection() {
        note('testing /ping ...');
        fetch('/ping', { cache: 'no-store' })
            .then(r => { note('PING HTTP ' + r.status); return r.text(); })
            .then(t => note('PING OK -> ' + t))
            .catch(e => note('PING FAILED -> ' + e.message));
    }

    function sendBrightness(v) {
        document.getElementById('brVal').innerText = v;
        fetch('/brightness?value=' + v)
            .then(r => note('brightness OK ' + r.status))
            .catch(e => note('brightness FAILED -> ' + e.message));
    }

    function saveSettings() {
        triggerSaveFlash();
        alert('Settings Saved Successfully!');
    }
</script>
</body>
</html>
)rawliteral";

#endif