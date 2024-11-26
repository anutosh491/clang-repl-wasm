!DOCTYPE html>
<html lang="en">
<head>
  <title>WASM Clang test</title>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <style>
    pre { background: #eee; list-style: none; }
    .stderr { color: darkred; }
    .stdout { color: darkgreen; }
  </style>
</head>
<body>
  <pre id="editor" contenteditable="true">#include &lt;SDL2/SDL.h&gt;

// Initialize
#include <iostream>
using namespace std;
cout << "Hello World" << endl;
  <canvas id="canvas"></canvas>
  <p id="status">Waiting for compiler to load... (~55 MiB)</p>
  <button id="run">RUN</button>
  <pre id="history"></pre>
  <script type="module" defer>
    import CompilerModule from './Compiler.js';
    (async function () {
      let history = document.querySelector('#history');
      var Module = {
        'print': (text) => {
          let newline = document.createElement('li');
          newline.className = 'stdout';
          newline.innerText = text;
          history.appendChild(newline);
        },
        'printErr': (text) => {
          let newline = document.createElement('li');
          newline.className = 'stderr';
          newline.innerText = text;
          history.appendChild(newline);
        },
        'canvas': document.getElementById('canvas'),
      };

      let statusElem = document.querySelector('#status');
      var instance = await CompilerModule(Module);
      console.log(instance);

      statusElem.innerText = "Compiler loaded, initializing...";
      let result = instance._init();

      if (result != 0) {
        statusElem.innerText = `Failed to initialize compiler, error: ${result}`;
      } else {
        statusElem.innerText = "Compiler initialized and ready!";
      }

      let editor = document.querySelector('#editor');
      let runButton = document.querySelector('#run').onclick = () => {
        let code = instance.stringToNewUTF8(editor.innerText);
        history.innerText = "";
        let result = instance._parse(code);
        instance._free(code);
        instance._execute();
      };
    })();
  </script>
</body>
</html>
