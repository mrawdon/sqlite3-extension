const fs = require("fs");
const sourceFiles = fs.readdirSync("./src");

const bindingOut = fs.createWriteStream("./binding.gyp");

bindingOut.write(`
{
  'includes': ['deps/common.gypi'],
  'targets': [
    ${
      sourceFiles.map(f=>`{
        'target_name': '${f.substring(0,f.indexOf('.'))}',
        'dependencies': ['deps/sqlite3.gyp:sqlite3'],
        'conditions': [['sqlite3 == ""', { 'sources': ['src/${f}'] }]],
      }`).join()
    },
    {
      'target_name': 'place_resulting_binaries',
      'type': 'none',
      'dependencies': ['test_extension','csv','regexp'],
      'copies': [{
        'files': [${sourceFiles.map(f=>`'<(PRODUCT_DIR)/${f.substring(0,f.indexOf('.'))}.node'`).join()}],
        'destination': 'build',
      }],
    },
  ],
}

`);

