module.exports = [
  {
    "type": "heading",
    "defaultValue": "FuelWatch Settings"
  },
  {
    "type": "section",
    "items": [
      {
        "type": "select",
        "messageKey": "FuelType",
        "defaultValue": "E10",
        "label": "Fuel type",
        "description": "Fuel station/price data provided by ANWB© NL.<br>Copyright notice ©<br>Covers the follwing countries:<br>NL, BE, LU, DE, FR, ES, PT, IT, CH, DK, NO, SE, FI, GB, TR, UA, RU.",
        "options": [
          { "label": "E10 (Euro 95)", "value": "E10"    },
          { "label": "E5 (Super 98)", "value": "E5"     },
          { "label": "Diesel (B7)",   "value": "DIESEL" },
          { "label": "LPG / Autogas", "value": "LPG"    }
        ]
      },
      {
        "type": "select",
        "messageKey": "StationCount",
        "defaultValue": "10",
        "label": "Stations in list",
        "description": "How many nearby stations to show. All are allready fetched in one request, so this does not affect data use and merely limits the list.",
        "options": [
          { "label": "10 stations", "value": "10" },
          { "label": "20 stations", "value": "20" },
          { "label": "30 stations", "value": "30" }
        ]
      }
    ]
  },
  {
    "type": "submit",
    "defaultValue": "Save"
  }
];