const express = require('express');
const path = require('path');

const port = 3000;
const app = express();

app.listen(port, () => console.log(`Example app listening at http://localhost:${port}`));

app.use('/', express.static(path.join(__dirname, '../filesystem')));

app.get('/open', (req, res) => {
  console.log(`/open ${JSON.stringify(req.query)}`);
  res.send(true);
});

app.get('/drive', (req, res) => {
  console.log(`/drive ${JSON.stringify(req.query)}`);
  res.sendStatus(200);
});